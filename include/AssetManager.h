#pragma once

#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <typeindex>
#include <unordered_map>

#include "Export.h"
#include "Uuid.h"
#include "TypedPayload.h"
#include "AssetPackReader.h"
#include "PayloadRegistry.h"
#include "AssetCache.h"
#include "AsyncLoader.h"
#include "RuntimePipelineConfig.h"

namespace SnAPI::AssetPipeline
{

// Context passed to asset factories during loading
struct AssetLoadContext
{
    // The cooked payload (metadata)
    const TypedPayload& Cooked;

    // The asset info
    const AssetInfo& Info;

    // Function to load bulk chunks by index
    std::function<std::expected<std::vector<uint8_t>, std::string>(uint32_t)> LoadBulk;

    // Function to load bulk chunk info
    std::function<std::expected<AssetPackReader::BulkChunkInfo, std::string>(uint32_t)> GetBulkInfo;

    // Payload registry for deserialization (may be null if not configured)
    const PayloadRegistry* Registry;

    // Helper to deserialize the cooked payload using the registry
    // T must match the struct type for the payload's TypeId
    template<typename T>
    std::expected<T, std::string> DeserializeCooked() const
    {
        if (!Registry)
        {
            return std::unexpected("No PayloadRegistry configured - cannot deserialize");
        }

        const IPayloadSerializer* Serializer = Registry->Find(Cooked.PayloadType);
        if (!Serializer)
        {
            return std::unexpected("No serializer found for payload type: " + Cooked.PayloadType.ToString());
        }

        T Result{};
        if (!Serializer->DeserializeFromBytes(&Result, Cooked.Bytes.data(), Cooked.Bytes.size()))
        {
            return std::unexpected("Failed to deserialize payload");
        }

        return Result;
    }
};

// Type-erased deleter for unique_ptr<void>
using VoidDeleter = void(*)(void*);
using UniqueVoidPtr = std::unique_ptr<void, VoidDeleter>;

// Base interface for asset factories (type-erased)
class SNAPI_ASSETPIPELINE_API IAssetFactory
{
public:
    virtual ~IAssetFactory() = default;

    // The cooked payload type this factory handles
    virtual TypeId GetCookedPayloadType() const = 0;

    // Estimate size in bytes for caching (optional, default = sizeof result)
    virtual size_t EstimateSize(const AssetLoadContext& Context) const { return 0; }

    // Create runtime object from cooked data
    // Returns type-erased unique_ptr
    virtual std::expected<UniqueVoidPtr, std::string> Load(const AssetLoadContext& Context) = 0;
};

// Helper template for implementing factories
// RuntimeT = your game's runtime type (e.g., Texture2D)
// Games should inherit from this and implement DoLoad()
template<typename RuntimeT>
class TAssetFactory : public IAssetFactory
{
public:
    std::expected<UniqueVoidPtr, std::string> Load(const AssetLoadContext& Context) override
    {
        auto Result = DoLoad(Context);
        if (!Result.has_value())
        {
            return std::unexpected(Result.error());
        }

        // Wrap in type-erased unique_ptr
        RuntimeT* Ptr = new RuntimeT(std::move(*Result));
        return UniqueVoidPtr(Ptr, [](void* P) { delete static_cast<RuntimeT*>(P); });
    }

protected:
    // Override this in your factory
    virtual std::expected<RuntimeT, std::string> DoLoad(const AssetLoadContext& Context) = 0;
};

// Mount options for packs
struct SNAPI_ASSETPIPELINE_API PackMountOptions
{
    int32_t Priority = 0;           // Higher priority packs override lower (for overlays/patches)
    bool bLoadToMemory = false;     // Load entire pack to memory (for small packs)
    std::string MountPoint = "";    // Virtual path prefix (e.g., "/dlc1/")
};

// Asset manager configuration
struct SNAPI_ASSETPIPELINE_API AssetManagerConfig
{
    AssetCacheConfig CacheConfig;
    uint32_t AsyncLoaderThreads = 0;    // 0 = auto (hardware_concurrency - 1)
    bool bEnableHotReload = false;      // Watch for pack file changes
    std::chrono::milliseconds HotReloadPollInterval{500};

    // Source asset roots for transparent loading
    std::vector<SourceMountConfig> SourceRoots;

    // Directories to auto-discover and mount .snpak files from
    std::vector<std::string> PackSearchPaths;

    // Runtime pipeline config (empty PluginPaths = disabled)
    RuntimePipelineConfig PipelineConfig;

    // Enable transparent source asset loading
    bool bEnableSourceAssets = false;
};

// Asset manager - manages pack readers, caching, and provides Load<T>() API
class SNAPI_ASSETPIPELINE_API AssetManager
{
public:
    explicit AssetManager(const AssetManagerConfig& Config = {});
    ~AssetManager();

    // ========== Pack Management ==========

    // Mount a pack file with options
    std::expected<void, std::string> MountPack(const std::string& Path, const PackMountOptions& Options = {});

    // Unmount a pack file
    void UnmountPack(const std::string& Path);

    // Unmount all packs
    void UnmountAll();

    // Get all mounted pack paths (in priority order, highest first)
    std::vector<std::string> GetMountedPacks() const;

    // ========== Registry ==========

    // Get the payload registry for registering serializers
    // Serializers should be registered before loading assets
    PayloadRegistry& GetRegistry();
    const PayloadRegistry& GetRegistry() const;

    // ========== Factory Registration ==========

    // Register a factory for a runtime type
    // The factory's GetCookedPayloadType() determines which cooked payloads it handles
    template<typename RuntimeT>
    void RegisterFactory(std::unique_ptr<IAssetFactory> Factory)
    {
        RegisterFactoryImpl(std::type_index(typeid(RuntimeT)), std::move(Factory));
    }

    // ========== Synchronous Loading ==========

    // Load an asset by name, returning unique ownership
    // Does NOT use cache - always loads fresh
    template<typename T>
    std::expected<std::unique_ptr<T>, std::string> Load(const std::string& Name)
    {
        auto Result = LoadAnyByName(Name, std::type_index(typeid(T)));
        if (!Result.has_value())
        {
            return std::unexpected(Result.error());
        }
        T* TypedPtr = static_cast<T*>(Result->release());
        return std::unique_ptr<T>(TypedPtr);
    }

    // Load an asset by ID, returning unique ownership
    template<typename T>
    std::expected<std::unique_ptr<T>, std::string> Load(AssetId Id)
    {
        auto Result = LoadAnyById(Id, std::type_index(typeid(T)));
        if (!Result.has_value())
        {
            return std::unexpected(Result.error());
        }
        T* TypedPtr = static_cast<T*>(Result->release());
        return std::unique_ptr<T>(TypedPtr);
    }

    // ========== Cached Loading (Ref-Counted) ==========

    // Get a cached asset, loading if not present
    // Returns a ref-counted handle - asset stays in cache while any handle exists
    template<typename T>
    std::expected<AssetHandle<T>, std::string> Get(const std::string& Name)
    {
        auto IdResult = ResolveAssetId(Name, std::type_index(typeid(T)));
        if (!IdResult.has_value())
        {
            return std::unexpected(IdResult.error());
        }
        return GetById<T>(*IdResult);
    }

    template<typename T>
    std::expected<AssetHandle<T>, std::string> GetById(AssetId Id)
    {
        // Check cache first
        auto Handle = GetCache().Get<T>(Id);
        if (Handle.IsValid())
        {
            return Handle;
        }

        // Not in cache - load and insert
        auto LoadResult = Load<T>(Id);
        if (!LoadResult.has_value())
        {
            return std::unexpected(LoadResult.error());
        }

        // Insert into cache
        size_t SizeEstimate = EstimateAssetSize(Id, std::type_index(typeid(T)));
        return GetCache().Insert<T>(Id, std::move(*LoadResult), SizeEstimate);
    }

    // Check if an asset is currently cached
    template<typename T>
    bool IsCached(AssetId Id) const
    {
        return GetCache().Contains<T>(Id);
    }

    // ========== Async Loading ==========

    // Get the async loader for queuing background loads
    AsyncLoader& GetAsyncLoader();

    // Convenience wrapper for async loading
    template<typename T>
    AsyncLoadHandle LoadAsync(const std::string& Name,
                               ELoadPriority Priority = ELoadPriority::Normal,
                               AsyncLoadCallback<T> Callback = nullptr,
                               CancellationToken Token = {})
    {
        return GetAsyncLoader().LoadAsync<T>(Name, Priority, std::move(Callback), std::move(Token));
    }

    template<typename T>
    AsyncLoadHandle LoadAsync(AssetId Id,
                               ELoadPriority Priority = ELoadPriority::Normal,
                               AsyncLoadCallback<T> Callback = nullptr,
                               CancellationToken Token = {})
    {
        return GetAsyncLoader().LoadAsync<T>(Id, Priority, std::move(Callback), std::move(Token));
    }

    // ========== Asset Discovery ==========

    // Find an asset by name (searches all mounted packs, respects priority)
    std::expected<AssetInfo, std::string> FindAsset(const std::string& Name) const;

    // Find an asset by ID (searches all mounted packs)
    std::expected<AssetInfo, std::string> FindAsset(AssetId Id) const;

    // Find all variants of an asset by name
    std::vector<AssetInfo> FindAssetVariants(const std::string& Name) const;

    // List all assets in mounted packs
    std::vector<AssetInfo> ListAssets() const;

    // ========== Cache Management ==========

    AssetCache& GetCache();
    const AssetCache& GetCache() const;

    // Clear all unreferenced assets from cache
    size_t ClearUnreferencedCache();

    // Force clear entire cache (dangerous!)
    void ClearCache();

    // ========== Hot Reload (Development) ==========

    // Enable/disable hot reload watching
    void SetHotReloadEnabled(bool bEnabled);
    bool IsHotReloadEnabled() const;

    // Manually check for pack file changes
    // Returns list of packs that were reloaded
    std::vector<std::string> CheckForChanges();

    // Register callback for when assets are reloaded
    using HotReloadCallback = std::function<void(const std::vector<AssetId>&)>;
    void SetHotReloadCallback(HotReloadCallback Callback);

    // ========== Source Asset Management ==========

    // Add a source root for transparent loading
    void AddSourceRoot(const SourceMountConfig& Config);

    // Remove a source root
    void RemoveSourceRoot(const std::string& RootPath);

    // Register importer/cooker directly (without plugin DLL)
    // Requires bEnableSourceAssets = true in config
    void RegisterImporter(std::unique_ptr<IAssetImporter> Importer);
    void RegisterCooker(std::unique_ptr<IAssetCooker> Cooker);

    // Save all dirty runtime-pipelined assets to disk
    std::expected<void, std::string> SaveRuntimeAssets();

    // Get number of dirty (unsaved) runtime-pipelined assets
    uint32_t GetDirtyAssetCount() const;

    // ========== Internal (for AsyncLoader) ==========

    std::expected<UniqueVoidPtr, std::string> LoadAnyByName(const std::string& Name, std::type_index RuntimeType);
    std::expected<UniqueVoidPtr, std::string> LoadAnyById(AssetId Id, std::type_index RuntimeType);

    // Non-copyable
    AssetManager(const AssetManager&) = delete;
    AssetManager& operator=(const AssetManager&) = delete;

    // Movable
    AssetManager(AssetManager&&) noexcept;
    AssetManager& operator=(AssetManager&&) noexcept;

private:
    void RegisterFactoryImpl(std::type_index RuntimeType, std::unique_ptr<IAssetFactory> Factory);
    std::expected<AssetId, std::string> ResolveAssetId(const std::string& Name, std::type_index RuntimeType);
    size_t EstimateAssetSize(AssetId Id, std::type_index RuntimeType);

    std::expected<AssetId, std::string> TryPipelineSource(const std::string& Name);
    std::expected<UniqueVoidPtr, std::string> LoadFromRuntimePipeline(const std::string& Name, std::type_index RuntimeType);

    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace SnAPI::AssetPipeline
