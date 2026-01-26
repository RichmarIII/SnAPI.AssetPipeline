#pragma once

#include <any>
#include <atomic>
#include <condition_variable>
#include <expected>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <typeindex>
#include <vector>

#include "Export.h"
#include "Uuid.h"

namespace SnAPI::AssetPipeline
{

// Forward declarations
class AssetManager;

// Load priority levels
enum class ELoadPriority : uint32_t
{
    Low = 0,        // Background prefetch
    Normal = 1,     // Standard loading
    High = 2,       // Player-visible soon
    Critical = 3,   // Blocking gameplay
};

// Cancellation token for async operations
class SNAPI_ASSETPIPELINE_API CancellationToken
{
public:
    CancellationToken() : m_Cancelled(std::make_shared<std::atomic<bool>>(false)) {}

    void Cancel() { m_Cancelled->store(true); }
    bool IsCancelled() const { return m_Cancelled->load(); }

    // Create a linked token that cancels when either is cancelled
    static CancellationToken CreateLinked(const CancellationToken& A, const CancellationToken& B);

private:
    std::shared_ptr<std::atomic<bool>> m_Cancelled;
};

// Handle to a pending async load operation
class SNAPI_ASSETPIPELINE_API AsyncLoadHandle
{
public:
    AsyncLoadHandle() = default;
    AsyncLoadHandle(uint64_t Id, CancellationToken Token)
        : m_Id(Id), m_Token(std::move(Token)) {}

    uint64_t GetId() const { return m_Id; }
    void Cancel() { m_Token.Cancel(); }
    bool IsCancelled() const { return m_Token.IsCancelled(); }

    bool IsValid() const { return m_Id != 0; }

private:
    uint64_t m_Id = 0;
    CancellationToken m_Token;
};

// Result of an async load
template<typename T>
struct AsyncLoadResult
{
    std::unique_ptr<T> Asset;
    std::string Error;
    bool bCancelled = false;

    bool IsSuccess() const { return Asset != nullptr && Error.empty() && !bCancelled; }
};

// Callback types
template<typename T>
using AsyncLoadCallback = std::function<void(AsyncLoadResult<T>)>;

using AsyncLoadCallbackVoid = std::function<void(bool bSuccess, const std::string& Error)>;

// Internal load request (type-erased)
struct LoadRequest
{
    uint64_t Id = 0;
    Uuid TargetAssetId;         // Asset ID if loading by ID
    std::string Name;           // If loading by name
    std::type_index RuntimeType;
    ELoadPriority Priority = ELoadPriority::Normal;
    std::any Params;            // User-supplied parameters passed to factory
    CancellationToken Token;
    std::function<void(void*, const std::string&)> Callback;  // void* = raw asset ptr
    std::function<void(void*)> ResultDeleter;                 // Type-erased deleter for cleanup on cancellation
    std::chrono::steady_clock::time_point QueueTime;

    // Priority queue comparison (higher priority first, then earlier queue time)
    bool operator<(const LoadRequest& Other) const
    {
        if (Priority != Other.Priority)
            return Priority < Other.Priority;  // Lower priority value = lower in queue
        return QueueTime > Other.QueueTime;    // Earlier time = higher priority
    }

    LoadRequest() : RuntimeType(typeid(void)) {}
};

// Async loader with thread pool and priority queue
class SNAPI_ASSETPIPELINE_API AsyncLoader
{
public:
    explicit AsyncLoader(AssetManager& Manager, uint32_t NumThreads = 0);
    ~AsyncLoader();

    // Queue an async load by name
    template<typename T>
    AsyncLoadHandle LoadAsync(const std::string& Name,
                               ELoadPriority Priority,
                               std::any Params,
                               AsyncLoadCallback<T> Callback,
                               CancellationToken Token = {});

    // Queue an async load by ID
    template<typename T>
    AsyncLoadHandle LoadAsync(AssetId Id,
                               ELoadPriority Priority,
                               std::any Params,
                               AsyncLoadCallback<T> Callback,
                               CancellationToken Token = {});

    // Blocking wait for a specific load to complete
    void Wait(const AsyncLoadHandle& Handle);

    // Wait for all pending loads to complete
    void WaitAll();

    // Cancel all pending loads
    void CancelAll();

    // Get statistics
    uint32_t GetPendingCount() const;
    uint32_t GetCompletedCount() const;

    // Process completed callbacks on calling thread (for main thread dispatch)
    // Returns number of callbacks processed
    uint32_t ProcessCompletedCallbacks();

    // Shutdown the loader (cancels pending, waits for workers)
    void Shutdown();

    // Non-copyable
    AsyncLoader(const AsyncLoader&) = delete;
    AsyncLoader& operator=(const AsyncLoader&) = delete;

private:
    void WorkerThread();
    uint64_t GenerateRequestId();

    AssetManager& m_Manager;

    std::vector<std::thread> m_Workers;
    std::priority_queue<LoadRequest> m_Queue;
    mutable std::mutex m_QueueMutex;
    std::condition_variable m_QueueCV;

    std::atomic<bool> m_Shutdown{false};
    std::atomic<uint64_t> m_NextRequestId{1};
    std::atomic<uint32_t> m_CompletedCount{0};

    // Completed callbacks waiting for main thread dispatch
    struct CompletedCallback
    {
        uint64_t RequestId;
        std::function<void()> Callback;
    };
    std::vector<CompletedCallback> m_CompletedCallbacks;
    std::mutex m_CompletedMutex;

    // Active requests for Wait() - FIX #2: Use shared_future for multiple waits
    struct WaitableRequest
    {
        std::shared_ptr<std::promise<void>> Promise;
        std::shared_future<void> Future;
    };
    std::unordered_map<uint64_t, WaitableRequest> m_ActiveRequests;
    std::mutex m_ActiveMutex;
};

// Template implementations
template<typename T>
AsyncLoadHandle AsyncLoader::LoadAsync(const std::string& Name,
                                        ELoadPriority Priority,
                                        std::any Params,
                                        AsyncLoadCallback<T> Callback,
                                        CancellationToken Token)
{
    LoadRequest Req;
    Req.Id = GenerateRequestId();
    Req.Name = Name;
    Req.RuntimeType = std::type_index(typeid(T));
    Req.Priority = Priority;
    Req.Params = std::move(Params);
    Req.Token = Token;
    Req.QueueTime = std::chrono::steady_clock::now();

    // Type-erased callback wrapper
    Req.Callback = [Callback = std::move(Callback)](void* RawPtr, const std::string& Error) {
        AsyncLoadResult<T> Result;
        if (RawPtr)
        {
            Result.Asset.reset(static_cast<T*>(RawPtr));
        }
        Result.Error = Error;
        Result.bCancelled = Error == "Cancelled";
        Callback(std::move(Result));
    };

    AsyncLoadHandle Handle(Req.Id, Token);

    // FIX #2: Populate m_ActiveRequests so Wait() can find this request
    {
        std::lock_guard ActiveLock(m_ActiveMutex);
        WaitableRequest Waitable;
        Waitable.Promise = std::make_shared<std::promise<void>>();
        Waitable.Future = Waitable.Promise->get_future().share();
        m_ActiveRequests[Req.Id] = std::move(Waitable);
    }

    {
        std::lock_guard Lock(m_QueueMutex);
        m_Queue.push(std::move(Req));
    }
    m_QueueCV.notify_one();

    return Handle;
}

template<typename T>
AsyncLoadHandle AsyncLoader::LoadAsync(AssetId Id,
                                        ELoadPriority Priority,
                                        std::any Params,
                                        AsyncLoadCallback<T> Callback,
                                        CancellationToken Token)
{
    LoadRequest Req;
    Req.Id = GenerateRequestId();
    Req.TargetAssetId = Id;
    Req.RuntimeType = std::type_index(typeid(T));
    Req.Priority = Priority;
    Req.Params = std::move(Params);
    Req.Token = Token;
    Req.QueueTime = std::chrono::steady_clock::now();

    Req.Callback = [Callback = std::move(Callback)](void* RawPtr, const std::string& Error) {
        AsyncLoadResult<T> Result;
        if (RawPtr)
        {
            Result.Asset.reset(static_cast<T*>(RawPtr));
        }
        Result.Error = Error;
        Result.bCancelled = Error == "Cancelled";
        Callback(std::move(Result));
    };

    AsyncLoadHandle Handle(Req.Id, Token);

    // FIX #2: Populate m_ActiveRequests so Wait() can find this request
    {
        std::lock_guard ActiveLock(m_ActiveMutex);
        WaitableRequest Waitable;
        Waitable.Promise = std::make_shared<std::promise<void>>();
        Waitable.Future = Waitable.Promise->get_future().share();
        m_ActiveRequests[Req.Id] = std::move(Waitable);
    }

    {
        std::lock_guard Lock(m_QueueMutex);
        m_Queue.push(std::move(Req));
    }
    m_QueueCV.notify_one();

    return Handle;
}

} // namespace SnAPI::AssetPipeline
