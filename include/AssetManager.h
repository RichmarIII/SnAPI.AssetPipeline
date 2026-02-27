#pragma once

#include <any>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <vector>

#include "Export.h"
#include "Uuid.h"
#include "TypedPayload.h"
#include "AssetPackReader.h"
#include "PayloadRegistry.h"
#include "AssetCache.h"
#include "AsyncLoader.h"
#include "SourceMountConfig.h"
#include "PipelineBuildConfig.h"

namespace SnAPI::AssetPipeline
{

struct CookedAsset;

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

    // Payload registry for deserialization
    const PayloadRegistry& Registry;

    // User-supplied parameters, factories cast to their expected type
    std::any Params;

    // Helper to deserialize the cooked payload using the registry
    // T must match the struct type for the payload's TypeId
    template<typename T>
    std::expected<T, std::string> DeserializeCooked() const
    {
        try
        {
            const IPayloadSerializer* Serializer = Registry.Find(Cooked.PayloadType);
            if (!Serializer)
            {
                return std::unexpected("No serializer found for payload type: " + Cooked.PayloadType.ToString());
            }

            std::vector<uint8_t> Bytes = Cooked.Bytes;
            const uint32_t TargetSchemaVersion = Serializer->GetSchemaVersion();
            if (Cooked.SchemaVersion != TargetSchemaVersion &&
                !Serializer->MigrateBytes(Cooked.SchemaVersion, TargetSchemaVersion, Bytes))
            {
                return std::unexpected("Payload schema mismatch and serializer migration failed: from " +
                                       std::to_string(Cooked.SchemaVersion) + " to " +
                                       std::to_string(TargetSchemaVersion));
            }

            T Result{};
            if (!Serializer->DeserializeFromBytes(&Result, Bytes.data(), Bytes.size()))
            {
                std::string Error = "Failed to deserialize payload bytes for type " + Cooked.PayloadType.ToString() +
                                    " at schema " + std::to_string(Cooked.SchemaVersion);
                if (Cooked.SchemaVersion == TargetSchemaVersion)
                {
                    Error += " (schema versions match; possible schema change without version bump)";
                }
                return std::unexpected(std::move(Error));
            }

            return Result;
        }
        catch (const std::exception& Ex)
        {
            return std::unexpected("Exception while deserializing payload: " + std::string(Ex.what()));
        }
        catch (...)
        {
            return std::unexpected("Unknown exception while deserializing payload");
        }
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
    AssetPackReadOptions ReadOptions;
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

    // Pipeline config (for on-demand processing and registry ownership)
    PipelineBuildConfig PipelineConfig;

    // Enable transparent source asset loading
    bool bEnableSourceAssets = false;

    // Auto-save dirty assets on AssetManager destruction
    bool bAutoSave = false;

    // When true, load failures throw after warning diagnostics are emitted.
    // Intended for debug/CI fail-fast workflows.
    bool bFatalOnLoadError = false;
};

enum class EAssetOrigin : std::uint8_t
{
    Pack = 0,
    RuntimeMemory = 1,
};

struct SNAPI_ASSETPIPELINE_API RuntimeAssetUpsert
{
    AssetId Id{};
    std::string Name{};
    TypeId AssetKind{};
    TypedPayload Cooked{};
    std::vector<BulkChunk> Bulk{};
    bool Dirty = true;
};

struct SNAPI_ASSETPIPELINE_API AssetCatalogEntry
{
    AssetInfo Info{};
    EAssetOrigin Origin = EAssetOrigin::Pack;
    bool Dirty = false;
    bool CanSave = false;
    std::string OwningPackPath{};
};

// Asset manager - manages pack readers, caching, and provides Load<T>() API
class SNAPI_ASSETPIPELINE_API AssetManager
{
public:
    using LoadWarningObserver = std::function<void(const AssetInfo* Info, const std::string& Message)>;
    using PayloadMigrationFn = std::function<std::expected<void, std::string>(std::vector<uint8_t>& InOutBytes)>;
    using PayloadMigrationObserver =
        std::function<void(const AssetInfo& Info,
                           TypeId PayloadType,
                           uint32_t FromVersion,
                           uint32_t ToVersion,
                           bool Success,
                           const std::string& Message)>;

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
    std::expected<std::unique_ptr<T>, std::string> Load(const std::string& Name, std::any Params = {})
    {
        auto Result = LoadAnyByName(Name, std::type_index(typeid(T)), std::move(Params));
        if (!Result.has_value())
        {
            return std::unexpected(Result.error());
        }
        T* TypedPtr = static_cast<T*>(Result->release());
        return std::unique_ptr<T>(TypedPtr);
    }

    // Load an asset by ID, returning unique ownership
    template<typename T>
    std::expected<std::unique_ptr<T>, std::string> Load(AssetId Id, std::any Params = {})
    {
        auto Result = LoadAnyById(Id, std::type_index(typeid(T)), std::move(Params));
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
    std::expected<AssetHandle<T>, std::string> Get(const std::string& Name, std::any Params = {})
    {
        auto IdResult = ResolveAssetId(Name, std::type_index(typeid(T)));
        if (!IdResult.has_value())
        {
            return std::unexpected(IdResult.error());
        }
        return GetById<T>(*IdResult, std::move(Params));
    }

    template<typename T>
    std::expected<AssetHandle<T>, std::string> GetById(AssetId Id, std::any Params = {})
    {
        // Check cache first
        auto Handle = GetCache().Get<T>(Id);
        if (Handle.IsValid())
        {
            return Handle;
        }

        // Not in cache - load and insert
        auto LoadResult = Load<T>(Id, std::move(Params));
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
                               std::any Params = {},
                               AsyncLoadCallback<T> Callback = nullptr,
                               CancellationToken Token = {})
    {
        return GetAsyncLoader().LoadAsync<T>(Name, Priority, std::move(Params), std::move(Callback), std::move(Token));
    }

    template<typename T>
    AsyncLoadHandle LoadAsync(AssetId Id,
                               ELoadPriority Priority = ELoadPriority::Normal,
                               std::any Params = {},
                               AsyncLoadCallback<T> Callback = nullptr,
                               CancellationToken Token = {})
    {
        return GetAsyncLoader().LoadAsync<T>(Id, Priority, std::move(Params), std::move(Callback), std::move(Token));
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

    // List all discovered assets with origin/dirty/save metadata.
    std::vector<AssetCatalogEntry> ListAssetCatalog() const;

    // Find a discovered asset by name with metadata.
    std::expected<AssetCatalogEntry, std::string> FindAssetCatalog(const std::string& Name) const;

    // Find a discovered asset by id with metadata.
    std::expected<AssetCatalogEntry, std::string> FindAssetCatalog(AssetId Id) const;

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

    // Register importer/cooker/serializer directly (without plugin DLL)
    void RegisterImporter(std::unique_ptr<IAssetImporter> Importer);
    void RegisterCooker(std::unique_ptr<IAssetCooker> Cooker);
    void RegisterSerializer(std::unique_ptr<IPayloadSerializer> Serializer);

    // Register a payload migration step for one schema version transition.
    // Migrations can be chained automatically (e.g. 1->2->3).
    void RegisterPayloadMigration(TypeId PayloadType, uint32_t FromVersion, uint32_t ToVersion, PayloadMigrationFn Callback);

    // Remove one migration step.
    void UnregisterPayloadMigration(TypeId PayloadType, uint32_t FromVersion, uint32_t ToVersion);

    // Remove all migration steps for one payload type.
    void ClearPayloadMigrations(TypeId PayloadType);

    // Remove all migration steps for all payload types.
    void ClearAllPayloadMigrations();

    // Observer invoked whenever a schema mismatch triggers migration handling.
    void SetOnPayloadMigration(PayloadMigrationObserver Callback);

    // Observer invoked when load/migration/deserialization fails.
    void SetOnLoadWarning(LoadWarningObserver Callback);

    // Enable/disable fail-fast behavior for load failures.
    void SetFatalOnLoadErrorEnabled(bool bEnabled);
    bool IsFatalOnLoadErrorEnabled() const;

    // Save all dirty runtime-pipelined assets to disk
    std::expected<void, std::string> SaveRuntimeAssets();

    // Get number of dirty (unsaved) runtime-pipelined assets
    uint32_t GetDirtyAssetCount() const;

    // Insert/update a runtime memory asset.
    std::expected<AssetId, std::string> UpsertRuntimeAsset(RuntimeAssetUpsert Asset);

    // Rename a runtime memory asset.
    std::expected<void, std::string> RenameRuntimeAsset(AssetId Id, const std::string& NewName);

    // Delete a runtime memory asset.
    std::expected<void, std::string> DeleteRuntimeAsset(AssetId Id);

    // Save a runtime memory asset to a pack path and mark it clean.
    std::expected<void, std::string> SaveRuntimeAsset(AssetId Id, const std::string& PackPath);

    // ========== Internal (for AsyncLoader) ==========

    std::expected<UniqueVoidPtr, std::string> LoadAnyByName(const std::string& Name, std::type_index RuntimeType, std::any Params = {});
    std::expected<UniqueVoidPtr, std::string> LoadAnyById(AssetId Id, std::type_index RuntimeType, std::any Params = {});

    // Non-copyable
    AssetManager(const AssetManager&) = delete;
    AssetManager& operator=(const AssetManager&) = delete;

    // Movable
    AssetManager(AssetManager&&) noexcept;
    AssetManager& operator=(AssetManager&&) noexcept;

private:
    std::expected<TypedPayload, std::string> ResolveCookedPayloadForLoad(const TypedPayload& Payload, const AssetInfo& Info) const;
    void ReportLoadWarning(const AssetInfo* Info, const std::string& Message) const;
    std::expected<UniqueVoidPtr, std::string> InvokeFactoryLoad(
        IAssetFactory& Factory,
        const AssetLoadContext& Context,
        const AssetInfo& Info) const;

    void RegisterFactoryImpl(std::type_index RuntimeType, std::unique_ptr<IAssetFactory> Factory);
    std::expected<AssetId, std::string> ResolveAssetId(const std::string& Name, std::type_index RuntimeType);
    size_t EstimateAssetSize(AssetId Id, std::type_index RuntimeType);

    std::expected<AssetId, std::string> TryPipelineSource(const std::string& Name);
    std::expected<UniqueVoidPtr, std::string> LoadFromRuntimeAsset(const CookedAsset& Asset, std::type_index RuntimeType, std::any Params = {});
    std::expected<UniqueVoidPtr, std::string> LoadFromRuntimePipeline(const std::string& Name, std::type_index RuntimeType, std::any Params = {});

    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace SnAPI::AssetPipeline
