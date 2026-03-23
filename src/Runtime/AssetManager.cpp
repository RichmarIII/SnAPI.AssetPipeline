#include "AssetManager.h"
#include "AssetPackWriter.h"
#include "AssetPipeline.h"
#include "Runtime/SourceAssetResolver.h"
#include "Runtime/AutoMountScanner.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <queue>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace SnAPI::AssetPipeline
{
  namespace
  {
    struct PayloadMigrationKey
    {
      TypeId PayloadType{};
      uint32_t FromVersion = 0;
      uint32_t ToVersion = 0;

      bool operator==(const PayloadMigrationKey& Other) const noexcept
      {
        return PayloadType == Other.PayloadType &&
               FromVersion == Other.FromVersion &&
               ToVersion == Other.ToVersion;
      }
    };

    struct PayloadMigrationKeyHash
    {
      size_t operator()(const PayloadMigrationKey& Key) const noexcept
      {
        const size_t TypeHash = UuidHash{}(Key.PayloadType);
        const size_t FromHash = std::hash<uint32_t>{}(Key.FromVersion);
        const size_t ToHash = std::hash<uint32_t>{}(Key.ToVersion);
        return TypeHash ^ (FromHash + 0x9e3779b9u + (TypeHash << 6) + (TypeHash >> 2)) ^
               (ToHash + 0x9e3779b9u + (TypeHash << 6) + (TypeHash >> 2));
      }
    };

    [[nodiscard]] AssetInfo ToAssetInfo(const CookedAsset& Asset)
    {
      AssetInfo Info{};
      Info.Id = Asset.Id;
      Info.AssetKind = Asset.AssetKind;
      Info.CookedPayloadType = Asset.Cooked.PayloadType;
      Info.SchemaVersion = Asset.Cooked.SchemaVersion;
      Info.Name = Asset.LogicalName;
      Info.VariantKey.clear();
      Info.BulkChunkCount = static_cast<uint32_t>(Asset.Bulk.size());
      Info.AssetDependencies = Asset.AssetDependencies;
      return Info;
    }

    [[nodiscard]] AssetPackEntry ToPackEntry(const CookedAsset& Asset)
    {
      AssetPackEntry Entry{};
      Entry.Id = Asset.Id;
      Entry.AssetKind = Asset.AssetKind;
      Entry.Name = Asset.LogicalName;
      Entry.Cooked = Asset.Cooked;
      Entry.Bulk = Asset.Bulk;
      Entry.AssetDependencies = Asset.AssetDependencies;
      return Entry;
    }
  } // namespace


  struct MountedPack
  {
      std::string Path;
      PackMountOptions Options;
      std::unique_ptr<AssetPackReader> Reader;
      std::filesystem::file_time_type LastModified;
  };

  struct AssetManager::Impl
  {
      AssetManagerConfig Config;

      // Asset cache
      std::unique_ptr<AssetCache> Cache;

      // Async loader (created lazily)
      std::unique_ptr<AsyncLoader> Loader;

      // Mounted pack readers (sorted by priority, highest first)
      std::vector<MountedPack> Packs;

      // Factories by runtime type. One runtime type may support multiple cooked payload shapes.
      std::unordered_map<std::type_index, std::vector<std::unique_ptr<IAssetFactory>>> FactoriesByRuntimeType;

      // Factories by cooked payload type (for lookup during load)
      std::unordered_map<TypeId, IAssetFactory*, UuidHash> FactoriesByCookedType;

      // Hot reload
      bool bHotReloadEnabled = false;
      AssetManager::HotReloadCallback HotReloadCallback;

      // Source asset support
      std::unique_ptr<SourceAssetResolver> SourceResolver;

      // Unified pipeline engine (owns PayloadRegistry)
      std::unique_ptr<AssetPipelineEngine> Engine;

      // Runtime memory assets (editor-authored/transient assets).
      mutable std::mutex RuntimeAssetsMutex{};
      std::unordered_map<AssetId, CookedAsset, UuidHash> RuntimeAssetsById{};
      std::unordered_map<std::string, AssetId> RuntimeAssetNameToId{};

      // Payload migration steps and optional observer.
      mutable std::mutex PayloadMigrationMutex{};
      std::unordered_map<PayloadMigrationKey, AssetManager::PayloadMigrationFn, PayloadMigrationKeyHash> PayloadMigrations{};
      AssetManager::PayloadMigrationObserver OnPayloadMigration{};
      AssetManager::LoadWarningObserver OnLoadWarning{};

      // Parent pointer for async loader
      AssetManager* Parent = nullptr;

      // Sort packs by priority (highest first)
      void SortPacks()
      {
        std::sort(Packs.begin(), Packs.end(), [](const MountedPack& A, const MountedPack& B) { return A.Options.Priority > B.Options.Priority; });
      }

      // Find which pack contains an asset by ID (respects priority order)
      std::pair<AssetPackReader*, const MountedPack*> FindPackForAsset(AssetId Id) const
      {
        for (const auto& Pack : Packs)
        {
          auto Result = Pack.Reader->FindAsset(Id);
          if (Result.has_value())
          {
            return {Pack.Reader.get(), &Pack};
          }
        }
        return {nullptr, nullptr};
      }

      // Find which pack contains an asset by name (respects priority order)
      std::tuple<AssetPackReader*, AssetInfo, const MountedPack*> FindPackForAssetByName(const std::string& Name) const
      {
        for (const auto& Pack : Packs)
        {
          // Apply mount point prefix if needed
          std::string LookupName = Name;
          if (!Pack.Options.MountPoint.empty())
          {
            if (Name.find(Pack.Options.MountPoint) == 0)
            {
              LookupName = Name.substr(Pack.Options.MountPoint.length());
            }
            else
            {
              continue; // Name doesn't match this pack's mount point
            }
          }

          auto Results = Pack.Reader->FindAssetsByName(LookupName);
          if (!Results.empty())
          {
            return {Pack.Reader.get(), Results[0], &Pack};
          }
        }
        return {nullptr, {}, nullptr};
      }

      bool TryFindRuntimeAssetById(const AssetId Id, CookedAsset& OutAsset) const
      {
        std::lock_guard Lock(RuntimeAssetsMutex);
        const auto It = RuntimeAssetsById.find(Id);
        if (It == RuntimeAssetsById.end())
        {
          return false;
        }
        OutAsset = It->second;
        return true;
      }

      bool TryFindRuntimeAssetByName(const std::string& Name, CookedAsset& OutAsset) const
      {
        std::lock_guard Lock(RuntimeAssetsMutex);
        const auto NameIt = RuntimeAssetNameToId.find(Name);
        if (NameIt == RuntimeAssetNameToId.end())
        {
          return false;
        }
        const auto AssetIt = RuntimeAssetsById.find(NameIt->second);
        if (AssetIt == RuntimeAssetsById.end())
        {
          return false;
        }
        OutAsset = AssetIt->second;
        return true;
      }
  };

  AssetManager::AssetManager(const AssetManagerConfig& Config) : m_Impl(std::make_unique<Impl>())
  {
    m_Impl->Config = Config;
    m_Impl->Cache = std::make_unique<AssetCache>(Config.CacheConfig);
    m_Impl->bHotReloadEnabled = Config.bEnableHotReload;
    m_Impl->Parent = this;

    // Always create the engine (owns the PayloadRegistry)
    m_Impl->Engine = std::make_unique<AssetPipelineEngine>();
    m_Impl->Engine->Initialize(Config.PipelineConfig);

    // Auto-mount packs from search paths
    if (!Config.PackSearchPaths.empty())
    {
      auto DiscoveredPacks = AutoMountScanner::Scan(Config.PackSearchPaths);
      for (const auto& PackPath : DiscoveredPacks)
      {
        MountPack(PackPath);
      }
    }

    // Initialize source asset support
    if (Config.bEnableSourceAssets)
    {
      m_Impl->SourceResolver = std::make_unique<SourceAssetResolver>();
      for (const auto& Root : Config.SourceRoots)
      {
        m_Impl->SourceResolver->AddRoot(Root);
      }

      // Mount existing runtime pack if configured and exists
      if (!Config.PipelineConfig.OutputPackPath.empty() &&
          std::filesystem::exists(Config.PipelineConfig.OutputPackPath))
      {
        PackMountOptions Options;
        Options.Priority = -50;
        MountPack(Config.PipelineConfig.OutputPackPath, Options);
      }
    }
  }

  AssetManager::~AssetManager()
  {
    // Shutdown async loader first
    if (m_Impl->Loader)
    {
      m_Impl->Loader->Shutdown();
    }

    // Auto-save dirty runtime assets if configured
    if (m_Impl->Engine && m_Impl->Engine->GetDirtyCount() > 0 && m_Impl->Config.bAutoSave)
    {
      SaveRuntimeAssets();
    }
  }

  AssetManager::AssetManager(AssetManager&&) noexcept = default;
  AssetManager& AssetManager::operator=(AssetManager&&) noexcept = default;

  std::expected<void, std::string> AssetManager::MountPack(const std::string& Path, const PackMountOptions& Options)
  {
    // Check if already mounted
    for (const auto& Pack : m_Impl->Packs)
    {
      if (Pack.Path == Path)
      {
        return std::unexpected("Pack already mounted: " + Path);
      }
    }

    auto Reader = std::make_unique<AssetPackReader>();
    auto Result = Reader->Open(Path, Options.ReadOptions);
    if (!Result.has_value())
    {
      return std::unexpected(Result.error());
    }

    MountedPack Pack;
    Pack.Path = Path;
    Pack.Options = Options;
    Pack.Reader = std::move(Reader);

    // Track modification time for hot reload
    try
    {
      Pack.LastModified = std::filesystem::last_write_time(Path);
    }
    catch (...)
    {
      // Ignore errors getting mod time
    }

    m_Impl->Packs.push_back(std::move(Pack));
    m_Impl->SortPacks();

    return {};
  }

  void AssetManager::UnmountPack(const std::string& Path)
  {
    auto It = std::find_if(m_Impl->Packs.begin(), m_Impl->Packs.end(), [&Path](const MountedPack& Pack) { return Pack.Path == Path; });

    if (It != m_Impl->Packs.end())
    {
      m_Impl->Packs.erase(It);
    }
  }

  void AssetManager::UnmountAll()
  {
    m_Impl->Packs.clear();
  }

  std::vector<std::string> AssetManager::GetMountedPacks() const
  {
    std::vector<std::string> Paths;
    Paths.reserve(m_Impl->Packs.size());
    for (const auto& Pack : m_Impl->Packs)
    {
      Paths.push_back(Pack.Path);
    }
    return Paths;
  }

  PayloadRegistry& AssetManager::GetRegistry()
  {
    return m_Impl->Engine->GetRegistry();
  }

  const PayloadRegistry& AssetManager::GetRegistry() const
  {
    return m_Impl->Engine->GetRegistry();
  }

  void AssetManager::RegisterFactoryImpl(std::type_index RuntimeType, std::unique_ptr<IAssetFactory> Factory)
  {
    TypeId CookedType = Factory->GetCookedPayloadType();
    IAssetFactory* RawPtr = Factory.get();

    m_Impl->FactoriesByRuntimeType[RuntimeType].push_back(std::move(Factory));
    m_Impl->FactoriesByCookedType[CookedType] = RawPtr;
  }

  IAssetFactory* AssetManager::ResolveFactory(const std::type_index RuntimeType, const TypeId CookedPayloadType) const
  {
    const auto FactoryIt = m_Impl->FactoriesByRuntimeType.find(RuntimeType);
    if (FactoryIt == m_Impl->FactoriesByRuntimeType.end())
    {
      return nullptr;
    }

    const auto& Factories = FactoryIt->second;
    for (const auto& Factory : Factories)
    {
      if (Factory && Factory->GetCookedPayloadType() == CookedPayloadType)
      {
        return Factory.get();
      }
    }

    return nullptr;
  }

  std::expected<AssetInfo, std::string> AssetManager::FindAsset(const std::string& Name) const
  {
    CookedAsset RuntimeAsset{};
    if (m_Impl->TryFindRuntimeAssetByName(Name, RuntimeAsset))
    {
      return ToAssetInfo(RuntimeAsset);
    }

    auto [Reader, Info, Pack] = m_Impl->FindPackForAssetByName(Name);
    if (!Reader)
    {
      return std::unexpected("Asset not found: " + Name);
    }
    return Info;
  }

  std::expected<AssetInfo, std::string> AssetManager::FindAsset(AssetId Id) const
  {
    CookedAsset RuntimeAsset{};
    if (m_Impl->TryFindRuntimeAssetById(Id, RuntimeAsset))
    {
      return ToAssetInfo(RuntimeAsset);
    }

    auto [Reader, Pack] = m_Impl->FindPackForAsset(Id);
    if (!Reader)
    {
      return std::unexpected("Asset not found: " + Id.ToString());
    }
    return Reader->FindAsset(Id);
  }

  std::vector<AssetInfo> AssetManager::FindAssetVariants(const std::string& Name) const
  {
    std::vector<AssetInfo> AllVariants;

    CookedAsset RuntimeAsset{};
    if (m_Impl->TryFindRuntimeAssetByName(Name, RuntimeAsset))
    {
      AllVariants.push_back(ToAssetInfo(RuntimeAsset));
    }

    for (const auto& Pack : m_Impl->Packs)
    {
      std::string LookupName = Name;
      if (!Pack.Options.MountPoint.empty())
      {
        if (Name.find(Pack.Options.MountPoint) == 0)
        {
          LookupName = Name.substr(Pack.Options.MountPoint.length());
        }
        else
        {
          continue;
        }
      }

      auto Variants = Pack.Reader->FindAssetsByName(LookupName);
      for (const auto& Variant : Variants)
      {
        if (std::find_if(AllVariants.begin(), AllVariants.end(), [&Variant](const AssetInfo& Existing) { return Existing.Id == Variant.Id; }) ==
            AllVariants.end())
        {
          AllVariants.push_back(Variant);
        }
      }
    }

    return AllVariants;
  }

  std::vector<AssetInfo> AssetManager::ListAssets() const
  {
    std::vector<AssetInfo> AllAssets;
    std::unordered_set<AssetId, UuidHash> SeenIds;

    {
      std::lock_guard Lock(m_Impl->RuntimeAssetsMutex);
      AllAssets.reserve(m_Impl->RuntimeAssetsById.size());
      for (const auto& [Id, Asset] : m_Impl->RuntimeAssetsById)
      {
        SeenIds.insert(Id);
        AllAssets.push_back(ToAssetInfo(Asset));
      }
    }

    for (const auto& Pack : m_Impl->Packs)
    {
      for (uint32_t I = 0; I < Pack.Reader->GetAssetCount(); ++I)
      {
        auto Info = Pack.Reader->GetAssetInfo(I);
        if (Info.has_value() && !SeenIds.contains(Info->Id))
        {
          SeenIds.insert(Info->Id);
          AllAssets.push_back(*Info);
        }
      }
    }

    return AllAssets;
  }

  std::vector<AssetCatalogEntry> AssetManager::ListAssetCatalog() const
  {
    std::vector<AssetCatalogEntry> Entries;
    std::unordered_set<AssetId, UuidHash> SeenIds;

    {
      std::lock_guard Lock(m_Impl->RuntimeAssetsMutex);
      Entries.reserve(m_Impl->RuntimeAssetsById.size() + m_Impl->Packs.size() * 8u);
      for (const auto& [Id, Asset] : m_Impl->RuntimeAssetsById)
      {
        SeenIds.insert(Id);
        AssetCatalogEntry Entry{};
        Entry.Info = ToAssetInfo(Asset);
        Entry.Origin = EAssetOrigin::RuntimeMemory;
        Entry.Dirty = Asset.bDirty;
        Entry.CanSave = true;
        Entries.push_back(std::move(Entry));
      }
    }

    for (const auto& Pack : m_Impl->Packs)
    {
      for (uint32_t I = 0; I < Pack.Reader->GetAssetCount(); ++I)
      {
        auto Info = Pack.Reader->GetAssetInfo(I);
        if (!Info.has_value() || SeenIds.contains(Info->Id))
        {
          continue;
        }

        SeenIds.insert(Info->Id);
        AssetCatalogEntry Entry{};
        Entry.Info = *Info;
        Entry.Origin = EAssetOrigin::Pack;
        Entry.Dirty = false;
        Entry.CanSave = true;
        Entry.OwningPackPath = Pack.Path;
        Entries.push_back(std::move(Entry));
      }
    }

    return Entries;
  }

  std::expected<AssetCatalogEntry, std::string> AssetManager::FindAssetCatalog(const std::string& Name) const
  {
    CookedAsset RuntimeAsset{};
    if (m_Impl->TryFindRuntimeAssetByName(Name, RuntimeAsset))
    {
      AssetCatalogEntry Entry{};
      Entry.Info = ToAssetInfo(RuntimeAsset);
      Entry.Origin = EAssetOrigin::RuntimeMemory;
      Entry.Dirty = RuntimeAsset.bDirty;
      Entry.CanSave = true;
      return Entry;
    }

    auto [Reader, Info, Pack] = m_Impl->FindPackForAssetByName(Name);
    if (!Reader)
    {
      return std::unexpected("Asset not found: " + Name);
    }

    AssetCatalogEntry Entry{};
    Entry.Info = Info;
    Entry.Origin = EAssetOrigin::Pack;
    Entry.Dirty = false;
    Entry.CanSave = true;
    if (Pack)
    {
      Entry.OwningPackPath = Pack->Path;
    }
    return Entry;
  }

  std::expected<AssetCatalogEntry, std::string> AssetManager::FindAssetCatalog(AssetId Id) const
  {
    CookedAsset RuntimeAsset{};
    if (m_Impl->TryFindRuntimeAssetById(Id, RuntimeAsset))
    {
      AssetCatalogEntry Entry{};
      Entry.Info = ToAssetInfo(RuntimeAsset);
      Entry.Origin = EAssetOrigin::RuntimeMemory;
      Entry.Dirty = RuntimeAsset.bDirty;
      Entry.CanSave = true;
      return Entry;
    }

    auto [Reader, Pack] = m_Impl->FindPackForAsset(Id);
    if (!Reader)
    {
      return std::unexpected("Asset not found: " + Id.ToString());
    }

    auto InfoResult = Reader->FindAsset(Id);
    if (!InfoResult.has_value())
    {
      return std::unexpected(InfoResult.error());
    }

    AssetCatalogEntry Entry{};
    Entry.Info = *InfoResult;
    Entry.Origin = EAssetOrigin::Pack;
    Entry.Dirty = false;
    Entry.CanSave = true;
    if (Pack)
    {
      Entry.OwningPackPath = Pack->Path;
    }
    return Entry;
  }

  std::expected<UniqueVoidPtr, std::string> AssetManager::LoadAnyByName(const std::string& Name, std::type_index RuntimeType, std::any Params)
  {
    CookedAsset RuntimeAsset{};
    if (m_Impl->TryFindRuntimeAssetByName(Name, RuntimeAsset))
    {
      return LoadFromRuntimeAsset(RuntimeAsset, RuntimeType, std::move(Params));
    }

    // Find the asset in mounted packs
    auto [Reader, Info, Pack] = m_Impl->FindPackForAssetByName(Name);

    // If not found and source assets enabled, try pipeline
    if (!Reader && m_Impl->SourceResolver)
    {
      auto PipeResult = TryPipelineSource(Name);
      if (PipeResult.has_value())
      {
        return LoadFromRuntimePipeline(Name, RuntimeType, std::move(Params));
      }
    }

    if (!Reader)
    {
      return std::unexpected("Asset not found: " + Name);
    }

    // Find factory for this runtime type
    IAssetFactory* Factory = ResolveFactory(RuntimeType, Info.CookedPayloadType);
    if (!Factory)
    {
      return std::unexpected("No factory registered for requested runtime type and cooked payload type " +
                             Info.CookedPayloadType.ToString());
    }

    // Load cooked payload
    auto PayloadResult = Reader->LoadCookedPayload(Info.Id);
    if (!PayloadResult.has_value())
    {
      return std::unexpected(PayloadResult.error());
    }

    auto MigratedPayloadResult = ResolveCookedPayloadForLoad(*PayloadResult, Info);
    if (!MigratedPayloadResult.has_value())
    {
      ReportLoadWarning(&Info, MigratedPayloadResult.error());
      return std::unexpected(MigratedPayloadResult.error());
    }
    TypedPayload CookedPayload = std::move(*MigratedPayloadResult);

    // Create load context
    AssetLoadContext Context{.Cooked = CookedPayload,
                             .Info = Info,
                             .LoadBulk = [Reader, Id = Info.Id](uint32_t Index) { return Reader->LoadBulkChunk(Id, Index); },
                             .GetBulkInfo = [Reader, Id = Info.Id](uint32_t Index) { return Reader->GetBulkChunkInfo(Id, Index); },
                             .Registry = m_Impl->Engine->GetRegistry(),
                             .Manager = this,
                             .Params = std::move(Params)};

    // Invoke factory
    return InvokeFactoryLoad(*Factory, Context, Info);
  }

  std::expected<UniqueVoidPtr, std::string> AssetManager::LoadAnyById(AssetId Id, std::type_index RuntimeType, std::any Params)
  {
    CookedAsset RuntimeAsset{};
    if (m_Impl->TryFindRuntimeAssetById(Id, RuntimeAsset))
    {
      return LoadFromRuntimeAsset(RuntimeAsset, RuntimeType, std::move(Params));
    }

    // Find the asset
    auto [Reader, Pack] = m_Impl->FindPackForAsset(Id);
    if (!Reader)
    {
      return std::unexpected("Asset not found: " + Id.ToString());
    }

    auto InfoResult = Reader->FindAsset(Id);
    if (!InfoResult.has_value())
    {
      return std::unexpected(InfoResult.error());
    }

    const AssetInfo& Info = *InfoResult;

    // Find factory for this runtime type
    IAssetFactory* Factory = ResolveFactory(RuntimeType, Info.CookedPayloadType);
    if (!Factory)
    {
      return std::unexpected("No factory registered for requested runtime type and cooked payload type " +
                             Info.CookedPayloadType.ToString());
    }

    // Load cooked payload
    auto PayloadResult = Reader->LoadCookedPayload(Id);
    if (!PayloadResult.has_value())
    {
      return std::unexpected(PayloadResult.error());
    }

    auto MigratedPayloadResult = ResolveCookedPayloadForLoad(*PayloadResult, Info);
    if (!MigratedPayloadResult.has_value())
    {
      ReportLoadWarning(&Info, MigratedPayloadResult.error());
      return std::unexpected(MigratedPayloadResult.error());
    }
    TypedPayload CookedPayload = std::move(*MigratedPayloadResult);

    // Create load context
    AssetLoadContext Context{.Cooked = CookedPayload,
                             .Info = Info,
                             .LoadBulk = [Reader, Id](uint32_t Index) { return Reader->LoadBulkChunk(Id, Index); },
                             .GetBulkInfo = [Reader, Id](uint32_t Index) { return Reader->GetBulkChunkInfo(Id, Index); },
                             .Registry = m_Impl->Engine->GetRegistry(),
                             .Manager = this,
                             .Params = std::move(Params)};

    // Invoke factory
    return InvokeFactoryLoad(*Factory, Context, Info);
  }

  std::expected<AssetId, std::string> AssetManager::ResolveAssetId(const std::string& Name, std::type_index RuntimeType)
  {
    auto Result = FindAsset(Name);
    if (!Result.has_value() && m_Impl->SourceResolver)
    {
      auto SourceResult = TryPipelineSource(Name);
      if (SourceResult.has_value())
      {
        return *SourceResult;
      }
    }
    if (!Result.has_value())
    {
      return std::unexpected(Result.error());
    }
    return Result->Id;
  }

  std::expected<AssetId, std::string> AssetManager::EnsureAssetFromSourcePayload(const SourcePayloadRequest& Request)
  {
    if (!Request.Id.IsNull())
    {
      auto ExistingById = FindAsset(Request.Id);
      if (ExistingById.has_value())
      {
        return ExistingById->Id;
      }
    }

    if (!Request.LogicalName.empty())
    {
      auto ExistingByName = FindAsset(Request.LogicalName);
      if (ExistingByName.has_value())
      {
        return ExistingByName->Id;
      }
    }

    if (Request.LogicalName.empty())
    {
      return std::unexpected("Source payload request logical name is empty");
    }

    SourcePayloadRequest LocalRequest = Request;
    auto Result = m_Impl->Engine->ProcessSourcePayload(std::move(LocalRequest));
    if (!Result.has_value())
    {
      return std::unexpected(Result.error());
    }

    auto CookedAssetResult = m_Impl->Engine->GetCookedAsset(Result->LogicalName);
    if (CookedAssetResult.has_value())
    {
      const auto& Cooked = CookedAssetResult->get();
      std::lock_guard Lock(m_Impl->RuntimeAssetsMutex);
      m_Impl->RuntimeAssetsById[Cooked.Id] = Cooked;
      m_Impl->RuntimeAssetNameToId[Cooked.LogicalName] = Cooked.Id;
    }

    return Result->Id;
  }

  size_t AssetManager::EstimateAssetSize(AssetId Id, std::type_index RuntimeType)
  {
    (void)RuntimeType;

    CookedAsset RuntimeAsset{};
    if (m_Impl->TryFindRuntimeAssetById(Id, RuntimeAsset))
    {
      size_t TotalSize = RuntimeAsset.Cooked.Bytes.size();
      for (const auto& Chunk : RuntimeAsset.Bulk)
      {
        TotalSize += Chunk.Bytes.size();
      }
      return TotalSize > 0 ? TotalSize : 1024;
    }

    // Try to get size from asset info
    auto [Reader, Pack] = m_Impl->FindPackForAsset(Id);
    if (!Reader)
    {
      return 0;
    }

    auto Info = Reader->FindAsset(Id);
    if (!Info.has_value())
    {
      return 0;
    }

    // Sum up bulk chunk sizes as estimate
    size_t TotalSize = 0;
    for (uint32_t I = 0; I < Info->BulkChunkCount; ++I)
    {
      auto BulkInfo = Reader->GetBulkChunkInfo(Id, I);
      if (BulkInfo.has_value())
      {
        TotalSize += BulkInfo->UncompressedSize;
      }
    }

    return TotalSize > 0 ? TotalSize : 1024; // Default to 1KB if unknown
  }

  AssetCache& AssetManager::GetCache()
  {
    return *m_Impl->Cache;
  }

  const AssetCache& AssetManager::GetCache() const
  {
    return *m_Impl->Cache;
  }

  size_t AssetManager::ClearUnreferencedCache()
  {
    return m_Impl->Cache->ClearUnreferenced();
  }

  size_t AssetManager::InvalidateAsset(const AssetId Id)
  {
    if (m_Impl->Engine)
    {
      (void)m_Impl->Engine->RemoveAsset(Id);
    }
    {
      std::lock_guard Lock(m_Impl->RuntimeAssetsMutex);
      if (const auto AssetIt = m_Impl->RuntimeAssetsById.find(Id); AssetIt != m_Impl->RuntimeAssetsById.end())
      {
        m_Impl->RuntimeAssetNameToId.erase(AssetIt->second.LogicalName);
        m_Impl->RuntimeAssetsById.erase(AssetIt);
      }
    }
    return m_Impl->Cache->RemoveAll(Id);
  }

  void AssetManager::ForceInvalidateAsset(const AssetId Id)
  {
    if (m_Impl->Engine)
    {
      (void)m_Impl->Engine->RemoveAsset(Id);
    }
    {
      std::lock_guard Lock(m_Impl->RuntimeAssetsMutex);
      if (const auto AssetIt = m_Impl->RuntimeAssetsById.find(Id); AssetIt != m_Impl->RuntimeAssetsById.end())
      {
        m_Impl->RuntimeAssetNameToId.erase(AssetIt->second.LogicalName);
        m_Impl->RuntimeAssetsById.erase(AssetIt);
      }
    }
    m_Impl->Cache->ForceRemoveAll(Id);
  }

  void AssetManager::ClearCache()
  {
    m_Impl->Cache->ClearAll();
  }

  AsyncLoader& AssetManager::GetAsyncLoader()
  {
    if (!m_Impl->Loader)
    {
      m_Impl->Loader = std::make_unique<AsyncLoader>(*this, m_Impl->Config.AsyncLoaderThreads);
    }
    return *m_Impl->Loader;
  }

  void AssetManager::SetHotReloadEnabled(bool bEnabled)
  {
    m_Impl->bHotReloadEnabled = bEnabled;
  }

  bool AssetManager::IsHotReloadEnabled() const
  {
    return m_Impl->bHotReloadEnabled;
  }

  std::vector<std::string> AssetManager::CheckForChanges()
  {
    std::vector<std::string> ReloadedPacks;

    if (!m_Impl->bHotReloadEnabled)
    {
      return ReloadedPacks;
    }

    std::vector<AssetId> ReloadedAssets;

    for (auto& Pack : m_Impl->Packs)
    {
      try
      {
        auto CurrentModTime = std::filesystem::last_write_time(Pack.Path);
        if (CurrentModTime != Pack.LastModified)
        {
          // Pack file changed - reload it
          auto NewReader = std::make_unique<AssetPackReader>();
          auto Result = NewReader->Open(Pack.Path);
          if (Result.has_value())
          {
            // Collect asset IDs that were in this pack
            for (uint32_t I = 0; I < Pack.Reader->GetAssetCount(); ++I)
            {
              auto Info = Pack.Reader->GetAssetInfo(I);
              if (Info.has_value())
              {
                ReloadedAssets.push_back(Info->Id);

                // Invalidate cache entry
                // Note: We can't easily do this without knowing the runtime type
                // A more complete implementation would track this
              }
            }

            Pack.Reader = std::move(NewReader);
            Pack.LastModified = CurrentModTime;
            ReloadedPacks.push_back(Pack.Path);
          }
        }
      }
      catch (...)
      {
        // Ignore filesystem errors during hot reload check
      }
    }

    // Notify callback
    if (!ReloadedAssets.empty() && m_Impl->HotReloadCallback)
    {
      m_Impl->HotReloadCallback(ReloadedAssets);
    }

    return ReloadedPacks;
  }

  void AssetManager::SetHotReloadCallback(HotReloadCallback Callback)
  {
    m_Impl->HotReloadCallback = std::move(Callback);
  }

  // ========== Source Asset Management ==========

  void AssetManager::AddSourceRoot(const SourceMountConfig& Config)
  {
    if (m_Impl->SourceResolver)
    {
      m_Impl->SourceResolver->AddRoot(Config);
    }
  }

  void AssetManager::RemoveSourceRoot(const std::string& RootPath)
  {
    if (m_Impl->SourceResolver)
    {
      m_Impl->SourceResolver->RemoveRoot(RootPath);
    }
  }

  void AssetManager::RegisterImporter(std::unique_ptr<IAssetImporter> Importer)
  {
    m_Impl->Engine->RegisterImporter(std::move(Importer));
  }

  void AssetManager::RegisterCooker(std::unique_ptr<IAssetCooker> Cooker)
  {
    m_Impl->Engine->RegisterCooker(std::move(Cooker));
  }

  void AssetManager::RegisterSerializer(std::unique_ptr<IPayloadSerializer> Serializer)
  {
    m_Impl->Engine->RegisterSerializer(std::move(Serializer));
  }

  void AssetManager::RegisterPayloadMigration(
      const TypeId PayloadType,
      const uint32_t FromVersion,
      const uint32_t ToVersion,
      PayloadMigrationFn Callback)
  {
    if (PayloadType.IsNull() || FromVersion == ToVersion || !Callback)
    {
      return;
    }

    std::lock_guard Lock(m_Impl->PayloadMigrationMutex);
    m_Impl->PayloadMigrations[PayloadMigrationKey{
        .PayloadType = PayloadType,
        .FromVersion = FromVersion,
        .ToVersion = ToVersion}] = std::move(Callback);
  }

  void AssetManager::UnregisterPayloadMigration(const TypeId PayloadType, const uint32_t FromVersion, const uint32_t ToVersion)
  {
    std::lock_guard Lock(m_Impl->PayloadMigrationMutex);
    m_Impl->PayloadMigrations.erase(PayloadMigrationKey{
        .PayloadType = PayloadType,
        .FromVersion = FromVersion,
        .ToVersion = ToVersion});
  }

  void AssetManager::ClearPayloadMigrations(const TypeId PayloadType)
  {
    std::lock_guard Lock(m_Impl->PayloadMigrationMutex);
    for (auto It = m_Impl->PayloadMigrations.begin(); It != m_Impl->PayloadMigrations.end();)
    {
      if (It->first.PayloadType == PayloadType)
      {
        It = m_Impl->PayloadMigrations.erase(It);
      }
      else
      {
        ++It;
      }
    }
  }

  void AssetManager::ClearAllPayloadMigrations()
  {
    std::lock_guard Lock(m_Impl->PayloadMigrationMutex);
    m_Impl->PayloadMigrations.clear();
  }

  void AssetManager::SetOnPayloadMigration(PayloadMigrationObserver Callback)
  {
    std::lock_guard Lock(m_Impl->PayloadMigrationMutex);
    m_Impl->OnPayloadMigration = std::move(Callback);
  }

  void AssetManager::SetOnLoadWarning(LoadWarningObserver Callback)
  {
    std::lock_guard Lock(m_Impl->PayloadMigrationMutex);
    m_Impl->OnLoadWarning = std::move(Callback);
  }

  void AssetManager::SetFatalOnLoadErrorEnabled(const bool bEnabled)
  {
    m_Impl->Config.bFatalOnLoadError = bEnabled;
  }

  bool AssetManager::IsFatalOnLoadErrorEnabled() const
  {
    return m_Impl->Config.bFatalOnLoadError;
  }

  void AssetManager::ReportLoadWarning(const AssetInfo* Info, const std::string& Message) const
  {
    std::string Formatted = "Asset load warning";
    if (Info)
    {
      Formatted += " [name=" + Info->Name + ", id=" + Info->Id.ToString() +
                   ", payload=" + Info->CookedPayloadType.ToString() +
                   ", schema=" + std::to_string(Info->SchemaVersion) + "]";
    }
    Formatted += ": " + Message;

    LoadWarningObserver Observer{};
    {
      std::lock_guard Lock(m_Impl->PayloadMigrationMutex);
      Observer = m_Impl->OnLoadWarning;
    }

    if (Observer)
    {
      Observer(Info, Formatted);
    }
    else
    {
      std::fprintf(stderr, "[WARN] %s\n", Formatted.c_str());
    }

    if (m_Impl->Config.bFatalOnLoadError)
    {
      throw std::runtime_error(Formatted);
    }
  }

  std::expected<UniqueVoidPtr, std::string> AssetManager::InvokeFactoryLoad(
      IAssetFactory& Factory, const AssetLoadContext& Context, const AssetInfo& Info) const
  {
    try
    {
      auto LoadResult = Factory.Load(Context);
      if (!LoadResult.has_value())
      {
        ReportLoadWarning(&Info, LoadResult.error());
        return std::unexpected(LoadResult.error());
      }
      return LoadResult;
    }
    catch (const std::exception& Ex)
    {
      const std::string Error = "Factory load threw exception: " + std::string(Ex.what());
      ReportLoadWarning(&Info, Error);
      return std::unexpected(Error);
    }
    catch (...)
    {
      const std::string Error = "Factory load threw unknown exception";
      ReportLoadWarning(&Info, Error);
      return std::unexpected(Error);
    }
  }

  std::expected<TypedPayload, std::string> AssetManager::ResolveCookedPayloadForLoad(const TypedPayload& Payload, const AssetInfo& Info) const
  {
    const IPayloadSerializer* Serializer = m_Impl->Engine->GetRegistry().Find(Payload.PayloadType);
    if (!Serializer)
    {
      return std::unexpected("No serializer found for payload type: " + Payload.PayloadType.ToString());
    }

    const uint32_t TargetSchemaVersion = Serializer->GetSchemaVersion();
    if (Payload.SchemaVersion == TargetSchemaVersion)
    {
      return Payload;
    }

    PayloadMigrationObserver Observer{};
    auto NotifyMigration = [&](const uint32_t FromVersion,
                               const uint32_t ToVersion,
                               const bool Success,
                               std::string Message) {
      {
        std::lock_guard Lock(m_Impl->PayloadMigrationMutex);
        Observer = m_Impl->OnPayloadMigration;
      }
      if (Observer)
      {
        Observer(Info, Payload.PayloadType, FromVersion, ToVersion, Success, Message);
      }
    };

    uint32_t CurrentVersion = Payload.SchemaVersion;
    std::vector<uint8_t> Bytes = Payload.Bytes;

    std::vector<std::pair<uint32_t, PayloadMigrationFn>> MigrationRoute{};
    {
      std::lock_guard Lock(m_Impl->PayloadMigrationMutex);

      using MigrationStep = std::pair<uint32_t, PayloadMigrationFn>;
      std::unordered_map<uint32_t, std::vector<MigrationStep>> Adjacency{};
      for (const auto& [Key, Callback] : m_Impl->PayloadMigrations)
      {
        if (Key.PayloadType != Payload.PayloadType || !Callback)
        {
          continue;
        }
        Adjacency[Key.FromVersion].emplace_back(Key.ToVersion, Callback);
      }

      if (!Adjacency.empty())
      {
        std::queue<uint32_t> Queue{};
        std::unordered_map<uint32_t, uint32_t> PreviousNode{};
        std::unordered_map<uint32_t, PayloadMigrationFn> PreviousStep{};

        Queue.push(CurrentVersion);
        PreviousNode.emplace(CurrentVersion, CurrentVersion);

        while (!Queue.empty() && !PreviousNode.contains(TargetSchemaVersion))
        {
          const uint32_t Node = Queue.front();
          Queue.pop();

          const auto AdjIt = Adjacency.find(Node);
          if (AdjIt == Adjacency.end())
          {
            continue;
          }

          for (const auto& [NextVersion, Callback] : AdjIt->second)
          {
            if (PreviousNode.contains(NextVersion))
            {
              continue;
            }

            PreviousNode.emplace(NextVersion, Node);
            PreviousStep.emplace(NextVersion, Callback);
            Queue.push(NextVersion);
          }
        }

        if (PreviousNode.contains(TargetSchemaVersion))
        {
          std::vector<MigrationStep> ReverseRoute{};
          for (uint32_t Cursor = TargetSchemaVersion; Cursor != CurrentVersion; Cursor = PreviousNode[Cursor])
          {
            const auto StepIt = PreviousStep.find(Cursor);
            if (StepIt == PreviousStep.end())
            {
              ReverseRoute.clear();
              break;
            }
            ReverseRoute.emplace_back(Cursor, StepIt->second);
          }

          if (!ReverseRoute.empty())
          {
            std::reverse(ReverseRoute.begin(), ReverseRoute.end());
            MigrationRoute = std::move(ReverseRoute);
          }
        }
      }
    }

    if (!MigrationRoute.empty())
    {
      for (const auto& [NextVersion, Callback] : MigrationRoute)
      {
        const uint32_t StepFrom = CurrentVersion;
        std::expected<void, std::string> StepResult{};
        try
        {
          StepResult = Callback(Bytes);
        }
        catch (const std::exception& Ex)
        {
          const std::string Error = "Payload migration callback threw for " + Payload.PayloadType.ToString() +
                                    " (" + std::to_string(StepFrom) + " -> " + std::to_string(NextVersion) + "): " +
                                    Ex.what();
          NotifyMigration(StepFrom, NextVersion, false, Error);
          return std::unexpected(Error);
        }
        catch (...)
        {
          const std::string Error = "Payload migration callback threw unknown exception for " + Payload.PayloadType.ToString() +
                                    " (" + std::to_string(StepFrom) + " -> " + std::to_string(NextVersion) + ")";
          NotifyMigration(StepFrom, NextVersion, false, Error);
          return std::unexpected(Error);
        }
        if (!StepResult.has_value())
        {
          const std::string Error = "Payload migration callback failed for " + Payload.PayloadType.ToString() +
                                    " (" + std::to_string(StepFrom) + " -> " + std::to_string(NextVersion) + "): " +
                                    StepResult.error();
          NotifyMigration(StepFrom, NextVersion, false, Error);
          return std::unexpected(Error);
        }

        CurrentVersion = NextVersion;
        NotifyMigration(StepFrom, NextVersion, true, "Applied registered payload migration callback");
      }
    }

    if (CurrentVersion != TargetSchemaVersion)
    {
      const uint32_t StepFrom = CurrentVersion;
      bool SerializerMigrationResult = false;
      try
      {
        SerializerMigrationResult = Serializer->MigrateBytes(CurrentVersion, TargetSchemaVersion, Bytes);
      }
      catch (const std::exception& Ex)
      {
        const std::string Error = "Serializer migration threw exception from schema " + std::to_string(CurrentVersion) +
                                  " to " + std::to_string(TargetSchemaVersion) + " for payload type " +
                                  Payload.PayloadType.ToString() + ": " + Ex.what();
        NotifyMigration(StepFrom, TargetSchemaVersion, false, Error);
        return std::unexpected(Error);
      }
      catch (...)
      {
        const std::string Error = "Serializer migration threw unknown exception from schema " + std::to_string(CurrentVersion) +
                                  " to " + std::to_string(TargetSchemaVersion) + " for payload type " +
                                  Payload.PayloadType.ToString();
        NotifyMigration(StepFrom, TargetSchemaVersion, false, Error);
        return std::unexpected(Error);
      }

      if (!SerializerMigrationResult)
      {
        const std::string Error = "No migration path from schema " + std::to_string(CurrentVersion) +
                                  " to " + std::to_string(TargetSchemaVersion) + " for payload type " +
                                  Payload.PayloadType.ToString();
        NotifyMigration(StepFrom, TargetSchemaVersion, false, Error);
        return std::unexpected(Error);
      }

      CurrentVersion = TargetSchemaVersion;
      NotifyMigration(StepFrom, TargetSchemaVersion, true, "Applied serializer payload migration");
    }

    return TypedPayload(Payload.PayloadType, CurrentVersion, std::move(Bytes));
  }

  std::expected<void, std::string> AssetManager::SaveRuntimeAssets()
  {
    auto EngineSaveResult = m_Impl->Engine->SaveAll();
    if (!EngineSaveResult.has_value())
    {
      return EngineSaveResult;
    }

    std::vector<CookedAsset> DirtyRuntimeAssets{};
    {
      std::lock_guard Lock(m_Impl->RuntimeAssetsMutex);
      DirtyRuntimeAssets.reserve(m_Impl->RuntimeAssetsById.size());
      for (const auto& [Id, Asset] : m_Impl->RuntimeAssetsById)
      {
        if (Asset.bDirty)
        {
          DirtyRuntimeAssets.push_back(Asset);
        }
      }
    }

    if (DirtyRuntimeAssets.empty())
    {
      return {};
    }

    if (m_Impl->Config.PipelineConfig.OutputPackPath.empty())
    {
      return std::unexpected("No OutputPackPath configured - cannot save runtime memory assets");
    }

    AssetPackWriter Writer{};
    for (const auto& Asset : DirtyRuntimeAssets)
    {
      Writer.AddAsset(ToPackEntry(Asset));
    }

    std::expected<void, std::string> WriteResult{};
    if (std::filesystem::exists(m_Impl->Config.PipelineConfig.OutputPackPath))
    {
      WriteResult = Writer.AppendUpdate(m_Impl->Config.PipelineConfig.OutputPackPath);
    }
    else
    {
      WriteResult = Writer.Write(m_Impl->Config.PipelineConfig.OutputPackPath);
    }

    if (!WriteResult.has_value())
    {
      return std::unexpected("Failed to write runtime assets: " + WriteResult.error());
    }

    {
      std::lock_guard Lock(m_Impl->RuntimeAssetsMutex);
      for (const auto& Asset : DirtyRuntimeAssets)
      {
        auto It = m_Impl->RuntimeAssetsById.find(Asset.Id);
        if (It != m_Impl->RuntimeAssetsById.end())
        {
          It->second.bDirty = false;
        }
      }
    }

    return {};
  }

  uint32_t AssetManager::GetDirtyAssetCount() const
  {
    std::unordered_set<AssetId, UuidHash> DirtyAssetIds{};
    if (m_Impl->Engine)
    {
      for (const AssetId& Id : m_Impl->Engine->GetDirtyAssetIds())
      {
        DirtyAssetIds.insert(Id);
      }
    }

    std::lock_guard Lock(m_Impl->RuntimeAssetsMutex);
    for (const auto& [Id, Asset] : m_Impl->RuntimeAssetsById)
    {
      if (Asset.bDirty)
      {
        DirtyAssetIds.insert(Id);
      }
    }
    return static_cast<uint32_t>(DirtyAssetIds.size());
  }

  std::expected<AssetId, std::string> AssetManager::UpsertRuntimeAsset(RuntimeAssetUpsert Asset)
  {
    if (Asset.Name.empty())
    {
      return std::unexpected("Runtime asset name cannot be empty");
    }
    if (Asset.Cooked.PayloadType.IsNull())
    {
      return std::unexpected("Runtime asset cooked payload type cannot be null");
    }
    if (Asset.Id.IsNull())
    {
      Asset.Id = AssetId::Generate();
    }

    CookedAsset Cooked{};
    Cooked.Id = Asset.Id;
    Cooked.LogicalName = std::move(Asset.Name);
    Cooked.AssetKind = Asset.AssetKind;
    Cooked.Cooked = std::move(Asset.Cooked);
    Cooked.Bulk = std::move(Asset.Bulk);
    Cooked.bDirty = Asset.Dirty;

    std::lock_guard Lock(m_Impl->RuntimeAssetsMutex);

    const auto ExistingByName = m_Impl->RuntimeAssetNameToId.find(Cooked.LogicalName);
    if (ExistingByName != m_Impl->RuntimeAssetNameToId.end() && ExistingByName->second != Cooked.Id)
    {
      return std::unexpected("A different runtime asset already uses name: " + Cooked.LogicalName);
    }

    if (const auto ExistingAsset = m_Impl->RuntimeAssetsById.find(Cooked.Id); ExistingAsset != m_Impl->RuntimeAssetsById.end())
    {
      m_Impl->RuntimeAssetNameToId.erase(ExistingAsset->second.LogicalName);
    }

    m_Impl->RuntimeAssetNameToId[Cooked.LogicalName] = Cooked.Id;
    m_Impl->RuntimeAssetsById[Cooked.Id] = std::move(Cooked);
    return Asset.Id;
  }

  std::expected<void, std::string> AssetManager::RenameRuntimeAsset(const AssetId Id, const std::string& NewName)
  {
    if (NewName.empty())
    {
      return std::unexpected("Runtime asset name cannot be empty");
    }

    std::lock_guard Lock(m_Impl->RuntimeAssetsMutex);
    const auto AssetIt = m_Impl->RuntimeAssetsById.find(Id);
    if (AssetIt == m_Impl->RuntimeAssetsById.end())
    {
      return std::unexpected("Runtime asset not found: " + Id.ToString());
    }

    const auto NameIt = m_Impl->RuntimeAssetNameToId.find(NewName);
    if (NameIt != m_Impl->RuntimeAssetNameToId.end() && NameIt->second != Id)
    {
      return std::unexpected("A different runtime asset already uses name: " + NewName);
    }

    m_Impl->RuntimeAssetNameToId.erase(AssetIt->second.LogicalName);
    AssetIt->second.LogicalName = NewName;
    AssetIt->second.bDirty = true;
    m_Impl->RuntimeAssetNameToId[AssetIt->second.LogicalName] = AssetIt->first;
    return {};
  }

  std::expected<void, std::string> AssetManager::DeleteRuntimeAsset(const AssetId Id)
  {
    {
      std::lock_guard Lock(m_Impl->RuntimeAssetsMutex);
      const auto AssetIt = m_Impl->RuntimeAssetsById.find(Id);
      if (AssetIt == m_Impl->RuntimeAssetsById.end())
      {
        return std::unexpected("Runtime asset not found: " + Id.ToString());
      }

      m_Impl->RuntimeAssetNameToId.erase(AssetIt->second.LogicalName);
      m_Impl->RuntimeAssetsById.erase(AssetIt);
    }

    // Deleting by ID invalidates any typed cache entry for this asset.
    m_Impl->Cache->RemoveAll(Id);
    return {};
  }

  std::expected<void, std::string> AssetManager::SaveRuntimeAsset(const AssetId Id, const std::string& PackPath)
  {
    if (PackPath.empty())
    {
      return std::unexpected("PackPath cannot be empty");
    }

    CookedAsset RuntimeAsset{};
    {
      std::lock_guard Lock(m_Impl->RuntimeAssetsMutex);
      const auto It = m_Impl->RuntimeAssetsById.find(Id);
      if (It == m_Impl->RuntimeAssetsById.end())
      {
        return std::unexpected("Runtime asset not found: " + Id.ToString());
      }
      RuntimeAsset = It->second;
    }

    std::filesystem::path OutputPath(PackPath);
    if (OutputPath.has_parent_path())
    {
      std::error_code Error{};
      std::filesystem::create_directories(OutputPath.parent_path(), Error);
      if (Error)
      {
        return std::unexpected("Failed to create runtime asset output directory: " + Error.message());
      }
    }

    AssetPackWriter Writer{};
    Writer.AddAsset(ToPackEntry(RuntimeAsset));

    std::expected<void, std::string> WriteResult{};
    if (std::filesystem::exists(OutputPath))
    {
      WriteResult = Writer.AppendUpdate(OutputPath.string());
    }
    else
    {
      WriteResult = Writer.Write(OutputPath.string());
    }

    if (!WriteResult.has_value())
    {
      return std::unexpected("Failed to save runtime asset: " + WriteResult.error());
    }

    {
      std::lock_guard Lock(m_Impl->RuntimeAssetsMutex);
      auto It = m_Impl->RuntimeAssetsById.find(Id);
      if (It != m_Impl->RuntimeAssetsById.end())
      {
        It->second.bDirty = false;
      }
    }

    // Ensure destination pack is mounted so discovery immediately reflects saved data.
    if (std::find_if(m_Impl->Packs.begin(), m_Impl->Packs.end(), [&OutputPath](const MountedPack& Pack) { return Pack.Path == OutputPath.string(); }) ==
        m_Impl->Packs.end())
    {
      (void)MountPack(OutputPath.string());
    }

    return {};
  }

  std::expected<TypedPayload, std::string> AssetManager::LoadCookedPayload(const std::string& Name) const
  {
    if (Name.empty())
    {
      return std::unexpected("Asset name is empty");
    }

    CookedAsset RuntimeAsset{};
    if (m_Impl->TryFindRuntimeAssetByName(Name, RuntimeAsset))
    {
      auto PayloadResult = ResolveCookedPayloadForLoad(RuntimeAsset.Cooked, ToAssetInfo(RuntimeAsset));
      if (!PayloadResult.has_value())
      {
        return std::unexpected(PayloadResult.error());
      }
      return std::move(*PayloadResult);
    }

    const auto [Reader, Info, Pack] = m_Impl->FindPackForAssetByName(Name);
    (void)Pack;
    if (!Reader)
    {
      return std::unexpected("Asset not found: " + Name);
    }

    auto PayloadResult = Reader->LoadCookedPayload(Info.Id);
    if (!PayloadResult.has_value())
    {
      return std::unexpected(PayloadResult.error());
    }

    auto ResolvedPayload = ResolveCookedPayloadForLoad(*PayloadResult, Info);
    if (!ResolvedPayload.has_value())
    {
      return std::unexpected(ResolvedPayload.error());
    }

    return std::move(*ResolvedPayload);
  }

  std::expected<TypedPayload, std::string> AssetManager::LoadCookedPayload(const AssetId Id) const
  {
    if (Id.IsNull())
    {
      return std::unexpected("Asset id is null");
    }

    CookedAsset RuntimeAsset{};
    if (m_Impl->TryFindRuntimeAssetById(Id, RuntimeAsset))
    {
      auto PayloadResult = ResolveCookedPayloadForLoad(RuntimeAsset.Cooked, ToAssetInfo(RuntimeAsset));
      if (!PayloadResult.has_value())
      {
        return std::unexpected(PayloadResult.error());
      }
      return std::move(*PayloadResult);
    }

    const auto [Reader, Pack] = m_Impl->FindPackForAsset(Id);
    (void)Pack;
    if (!Reader)
    {
      return std::unexpected("Asset not found: " + Id.ToString());
    }

    auto InfoResult = Reader->FindAsset(Id);
    if (!InfoResult.has_value())
    {
      return std::unexpected(InfoResult.error());
    }

    auto PayloadResult = Reader->LoadCookedPayload(Id);
    if (!PayloadResult.has_value())
    {
      return std::unexpected(PayloadResult.error());
    }

    auto ResolvedPayload = ResolveCookedPayloadForLoad(*PayloadResult, *InfoResult);
    if (!ResolvedPayload.has_value())
    {
      return std::unexpected(ResolvedPayload.error());
    }

    return std::move(*ResolvedPayload);
  }

  std::expected<AssetId, std::string> AssetManager::TryPipelineSource(const std::string& Name)
  {
    if (m_Impl->Engine->HasAsset(Name))
    {
      auto ExistingId = m_Impl->Engine->GetAssetId(Name);
      if (!ExistingId.has_value())
      {
        return std::unexpected(ExistingId.error());
      }

      auto CookedAssetResult = m_Impl->Engine->GetCookedAsset(Name);
      if (CookedAssetResult.has_value())
      {
        const auto& Cooked = CookedAssetResult->get();
        std::lock_guard Lock(m_Impl->RuntimeAssetsMutex);
        m_Impl->RuntimeAssetsById[Cooked.Id] = Cooked;
        m_Impl->RuntimeAssetNameToId[Cooked.LogicalName] = Cooked.Id;
      }

      return ExistingId;
    }

    auto Resolved = m_Impl->SourceResolver->Resolve(Name);
    if (!Resolved)
    {
      return std::unexpected("Source not found: " + Name);
    }

    auto Result = m_Impl->Engine->ProcessSource(Resolved->AbsolutePath, Resolved->LogicalName);
    if (!Result.has_value())
    {
      return std::unexpected(Result.error());
    }

    auto CookedAssetResult = m_Impl->Engine->GetCookedAsset(Result->LogicalName);
    if (CookedAssetResult.has_value())
    {
      const auto& Cooked = CookedAssetResult->get();
      std::lock_guard Lock(m_Impl->RuntimeAssetsMutex);
      m_Impl->RuntimeAssetsById[Cooked.Id] = Cooked;
      m_Impl->RuntimeAssetNameToId[Cooked.LogicalName] = Cooked.Id;
    }

    return Result->Id;
  }

  std::expected<UniqueVoidPtr, std::string> AssetManager::LoadFromRuntimePipeline(
      const std::string& Name, std::type_index RuntimeType, std::any Params)
  {
    auto AssetResult = m_Impl->Engine->GetCookedAsset(Name);
    if (!AssetResult.has_value())
    {
      return std::unexpected(AssetResult.error());
    }
    return LoadFromRuntimeAsset(AssetResult->get(), RuntimeType, std::move(Params));
  }

  std::expected<UniqueVoidPtr, std::string> AssetManager::LoadFromRuntimeAsset(
      const CookedAsset& Asset, const std::type_index RuntimeType, std::any Params)
  {
    // Find factory for this runtime type
    IAssetFactory* Factory = ResolveFactory(RuntimeType, Asset.Cooked.PayloadType);
    if (!Factory)
    {
      return std::unexpected("No factory registered for requested runtime type and cooked payload type " +
                             Asset.Cooked.PayloadType.ToString());
    }

    // Build AssetInfo for the context
    const AssetInfo Info = ToAssetInfo(Asset);

    auto MigratedPayloadResult = ResolveCookedPayloadForLoad(Asset.Cooked, Info);
    if (!MigratedPayloadResult.has_value())
    {
      ReportLoadWarning(&Info, MigratedPayloadResult.error());
      return std::unexpected(MigratedPayloadResult.error());
    }
    TypedPayload CookedPayload = std::move(*MigratedPayloadResult);

    // Create load context with in-memory data
    const auto& BulkChunks = Asset.Bulk;
    AssetLoadContext Context{
        .Cooked = CookedPayload,
        .Info = Info,
        .LoadBulk = [&BulkChunks](uint32_t Index) -> std::expected<std::vector<uint8_t>, std::string> {
          if (Index >= BulkChunks.size())
          {
            return std::unexpected("Bulk index out of range");
          }
          return BulkChunks[Index].Bytes;
        },
        .GetBulkInfo = [&BulkChunks](uint32_t Index) -> std::expected<AssetPackReader::BulkChunkInfo, std::string> {
          if (Index >= BulkChunks.size())
          {
            return std::unexpected("Bulk index out of range");
          }
          return AssetPackReader::BulkChunkInfo{
              .Semantic = BulkChunks[Index].Semantic,
              .SubIndex = BulkChunks[Index].SubIndex,
              .UncompressedSize = BulkChunks[Index].Bytes.size(),
          };
        },
        .Registry = m_Impl->Engine->GetRegistry(),
        .Manager = this,
        .Params = std::move(Params)};

    return InvokeFactoryLoad(*Factory, Context, Info);
  }

} // namespace SnAPI::AssetPipeline
