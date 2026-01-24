#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <typeindex>
#include <unordered_map>

#include "Export.h"
#include "Uuid.h"

namespace SnAPI::AssetPipeline
{

// Forward declarations
class AssetCache;

// Type-erased deleter for cached assets
using AssetDeleter = void(*)(void*);

// Internal cache entry (type-erased)
struct CacheEntry
{
    AssetId Id;
    std::type_index Type;
    void* Asset = nullptr;
    AssetDeleter Deleter = nullptr;

    std::atomic<uint32_t> RefCount{0};
    std::chrono::steady_clock::time_point LastAccess;
    size_t SizeBytes = 0;  // Approximate memory size

    CacheEntry() : Type(typeid(void)) {}
    ~CacheEntry()
    {
        if (Asset && Deleter)
        {
            Deleter(Asset);
        }
    }

    CacheEntry(const CacheEntry&) = delete;
    CacheEntry& operator=(const CacheEntry&) = delete;
};

// Ref-counted handle to a cached asset
template<typename T>
class AssetHandle
{
public:
    AssetHandle() = default;

    AssetHandle(std::shared_ptr<CacheEntry> Entry)
        : m_Entry(std::move(Entry))
    {
        if (m_Entry)
        {
            m_Entry->RefCount.fetch_add(1);
        }
    }

    AssetHandle(const AssetHandle& Other)
        : m_Entry(Other.m_Entry)
    {
        if (m_Entry)
        {
            m_Entry->RefCount.fetch_add(1);
        }
    }

    AssetHandle(AssetHandle&& Other) noexcept
        : m_Entry(std::move(Other.m_Entry))
    {
        Other.m_Entry = nullptr;
    }

    ~AssetHandle()
    {
        Release();
    }

    AssetHandle& operator=(const AssetHandle& Other)
    {
        if (this != &Other)
        {
            Release();
            m_Entry = Other.m_Entry;
            if (m_Entry)
            {
                m_Entry->RefCount.fetch_add(1);
            }
        }
        return *this;
    }

    AssetHandle& operator=(AssetHandle&& Other) noexcept
    {
        if (this != &Other)
        {
            Release();
            m_Entry = std::move(Other.m_Entry);
            Other.m_Entry = nullptr;
        }
        return *this;
    }

    void Release()
    {
        if (m_Entry)
        {
            m_Entry->RefCount.fetch_sub(1);
            m_Entry = nullptr;
        }
    }

    T* Get() const
    {
        return m_Entry ? static_cast<T*>(m_Entry->Asset) : nullptr;
    }

    T* operator->() const { return Get(); }
    T& operator*() const { return *Get(); }

    explicit operator bool() const { return Get() != nullptr; }

    bool IsValid() const { return m_Entry && m_Entry->Asset; }
    bool IsUnique() const { return m_Entry && m_Entry->RefCount.load() == 1; }
    uint32_t UseCount() const { return m_Entry ? m_Entry->RefCount.load() : 0; }

    AssetId GetAssetId() const { return m_Entry ? m_Entry->Id : AssetId{}; }

private:
    std::shared_ptr<CacheEntry> m_Entry;
};

// Cache eviction policy
enum class ECacheEvictionPolicy
{
    LRU,        // Least Recently Used
    LFU,        // Least Frequently Used (based on refcount history)
    Size,       // Largest assets first
};

// Cache configuration
struct SNAPI_ASSETPIPELINE_API AssetCacheConfig
{
    size_t MaxMemoryBytes = 512 * 1024 * 1024;  // 512 MB default
    size_t EvictionThresholdBytes = 0;           // Start evicting at this level (0 = 90% of max)
    ECacheEvictionPolicy EvictionPolicy = ECacheEvictionPolicy::LRU;
    bool bEvictOnlyUnreferenced = true;          // Only evict assets with RefCount == 0
    std::chrono::seconds MinAgeBeforeEviction{5}; // Don't evict recently loaded assets
};

// LRU asset cache with ref-counting
class SNAPI_ASSETPIPELINE_API AssetCache
{
public:
    explicit AssetCache(const AssetCacheConfig& Config = {});
    ~AssetCache();

    // Check if an asset is in the cache
    bool Contains(AssetId Id, std::type_index Type) const;

    template<typename T>
    bool Contains(AssetId Id) const
    {
        return Contains(Id, std::type_index(typeid(T)));
    }

    // Get a cached asset (returns empty handle if not found)
    template<typename T>
    AssetHandle<T> Get(AssetId Id)
    {
        auto Entry = GetEntry(Id, std::type_index(typeid(T)));
        if (Entry)
        {
            Entry->LastAccess = std::chrono::steady_clock::now();
            return AssetHandle<T>(Entry);
        }
        return {};
    }

    // Insert an asset into the cache
    // Takes ownership of the asset
    template<typename T>
    AssetHandle<T> Insert(AssetId Id, std::unique_ptr<T> Asset, size_t SizeBytes = 0)
    {
        if (!Asset)
        {
            return {};
        }

        auto Entry = std::make_shared<CacheEntry>();
        Entry->Id = Id;
        Entry->Type = std::type_index(typeid(T));
        Entry->Asset = Asset.release();
        Entry->Deleter = [](void* P) { delete static_cast<T*>(P); };
        Entry->LastAccess = std::chrono::steady_clock::now();
        Entry->SizeBytes = SizeBytes > 0 ? SizeBytes : sizeof(T);

        InsertEntry(Entry);

        return AssetHandle<T>(Entry);
    }

    // Remove an asset from the cache (only if RefCount == 0)
    bool Remove(AssetId Id, std::type_index Type);

    template<typename T>
    bool Remove(AssetId Id)
    {
        return Remove(Id, std::type_index(typeid(T)));
    }

    // Force remove (even if referenced - dangerous!)
    void ForceRemove(AssetId Id, std::type_index Type);

    // Clear all unreferenced assets
    size_t ClearUnreferenced();

    // Clear all assets (dangerous - may leave dangling handles!)
    void ClearAll();

    // Run eviction if needed based on config
    size_t Evict();

    // Statistics
    size_t GetCachedCount() const;
    size_t GetMemoryUsage() const;
    size_t GetReferencedCount() const;

    // Configuration
    void SetConfig(const AssetCacheConfig& Config);
    const AssetCacheConfig& GetConfig() const { return m_Config; }

private:
    std::shared_ptr<CacheEntry> GetEntry(AssetId Id, std::type_index Type);
    void InsertEntry(std::shared_ptr<CacheEntry> Entry);
    void TouchLRU(const std::shared_ptr<CacheEntry>& Entry);

    AssetCacheConfig m_Config;

    // Cache storage: (AssetId, TypeIndex) -> Entry
    struct CacheKey
    {
        AssetId Id;
        std::type_index Type;

        CacheKey(AssetId InId, std::type_index InType) : Id(InId), Type(InType) {}

        bool operator==(const CacheKey& Other) const
        {
            return Id == Other.Id && Type == Other.Type;
        }
    };

    struct CacheKeyHash
    {
        size_t operator()(const CacheKey& Key) const
        {
            size_t H1 = UuidHash{}(Key.Id);
            size_t H2 = Key.Type.hash_code();
            return H1 ^ (H2 << 1);
        }
    };

    std::unordered_map<CacheKey, std::shared_ptr<CacheEntry>, CacheKeyHash> m_Cache;
    mutable std::shared_mutex m_CacheMutex;

    // LRU list (front = most recent, back = least recent)
    std::list<std::shared_ptr<CacheEntry>> m_LruList;
    std::unordered_map<CacheEntry*, std::list<std::shared_ptr<CacheEntry>>::iterator> m_LruMap;

    std::atomic<size_t> m_MemoryUsage{0};
};

} // namespace SnAPI::AssetPipeline
