#include "AssetManager.h"
#include "Runtime/SourceAssetResolver.h"
#include "Runtime/RuntimePipeline.h"
#include "Runtime/AutoMountScanner.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <unordered_map>
#include <vector>

namespace SnAPI::AssetPipeline
{

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

      // Payload registry for serialization
      PayloadRegistry Registry;

      // Asset cache
      std::unique_ptr<AssetCache> Cache;

      // Async loader (created lazily)
      std::unique_ptr<AsyncLoader> Loader;

      // Mounted pack readers (sorted by priority, highest first)
      std::vector<MountedPack> Packs;

      // Factories by runtime type
      std::unordered_map<std::type_index, std::unique_ptr<IAssetFactory>> FactoriesByRuntimeType;

      // Factories by cooked payload type (for lookup during load)
      std::unordered_map<TypeId, IAssetFactory*, UuidHash> FactoriesByCookedType;

      // Hot reload
      bool bHotReloadEnabled = false;
      AssetManager::HotReloadCallback HotReloadCallback;

      // Source asset support
      std::unique_ptr<SourceAssetResolver> SourceResolver;
      std::unique_ptr<RuntimePipeline> Pipeline;

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
  };

  AssetManager::AssetManager(const AssetManagerConfig& Config) : m_Impl(std::make_unique<Impl>())
  {
    m_Impl->Config = Config;
    m_Impl->Cache = std::make_unique<AssetCache>(Config.CacheConfig);
    m_Impl->bHotReloadEnabled = Config.bEnableHotReload;
    m_Impl->Parent = this;

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

      m_Impl->Pipeline = std::make_unique<RuntimePipeline>(Config.PipelineConfig);
      auto InitResult = m_Impl->Pipeline->Initialize();
      if (!InitResult.has_value())
      {
        // Pipeline init failed - disable source assets
        m_Impl->Pipeline.reset();
        m_Impl->SourceResolver.reset();
      }
      else
      {
        // If runtime pack exists on disk, mount it at low priority
        std::string RuntimePackPath;
        if (!Config.PipelineConfig.OutputDirectory.empty())
        {
          RuntimePackPath = (std::filesystem::path(Config.PipelineConfig.OutputDirectory) / Config.PipelineConfig.RuntimePackName).string();
        }
        else
        {
          RuntimePackPath = Config.PipelineConfig.RuntimePackName;
        }

        if (std::filesystem::exists(RuntimePackPath))
        {
          PackMountOptions Options;
          Options.Priority = -50;
          MountPack(RuntimePackPath, Options);
        }
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
    if (m_Impl->Pipeline && m_Impl->Pipeline->GetDirtyCount() > 0 && m_Impl->Config.PipelineConfig.bAutoSave)
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
    auto Result = Reader->Open(Path);
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
    return m_Impl->Registry;
  }

  const PayloadRegistry& AssetManager::GetRegistry() const
  {
    return m_Impl->Registry;
  }

  void AssetManager::RegisterFactoryImpl(std::type_index RuntimeType, std::unique_ptr<IAssetFactory> Factory)
  {
    TypeId CookedType = Factory->GetCookedPayloadType();
    IAssetFactory* RawPtr = Factory.get();

    m_Impl->FactoriesByRuntimeType[RuntimeType] = std::move(Factory);
    m_Impl->FactoriesByCookedType[CookedType] = RawPtr;
  }

  std::expected<AssetInfo, std::string> AssetManager::FindAsset(const std::string& Name) const
  {
    auto [Reader, Info, Pack] = m_Impl->FindPackForAssetByName(Name);
    if (!Reader)
    {
      return std::unexpected("Asset not found: " + Name);
    }
    return Info;
  }

  std::expected<AssetInfo, std::string> AssetManager::FindAsset(AssetId Id) const
  {
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
      AllVariants.insert(AllVariants.end(), Variants.begin(), Variants.end());
    }

    return AllVariants;
  }

  std::vector<AssetInfo> AssetManager::ListAssets() const
  {
    std::vector<AssetInfo> AllAssets;

    for (const auto& Pack : m_Impl->Packs)
    {
      for (uint32_t I = 0; I < Pack.Reader->GetAssetCount(); ++I)
      {
        auto Info = Pack.Reader->GetAssetInfo(I);
        if (Info.has_value())
        {
          AllAssets.push_back(*Info);
        }
      }
    }

    return AllAssets;
  }

  std::expected<UniqueVoidPtr, std::string> AssetManager::LoadAnyByName(const std::string& Name, std::type_index RuntimeType)
  {
    // Find the asset in mounted packs
    auto [Reader, Info, Pack] = m_Impl->FindPackForAssetByName(Name);

    // If not found and source assets enabled, try pipeline
    if (!Reader && m_Impl->SourceResolver && m_Impl->Pipeline)
    {
      auto PipeResult = TryPipelineSource(Name);
      if (PipeResult.has_value())
      {
        return LoadFromRuntimePipeline(Name, RuntimeType);
      }
    }

    if (!Reader)
    {
      return std::unexpected("Asset not found: " + Name);
    }

    // Find factory for this runtime type
    auto FactoryIt = m_Impl->FactoriesByRuntimeType.find(RuntimeType);
    if (FactoryIt == m_Impl->FactoriesByRuntimeType.end())
    {
      return std::unexpected("No factory registered for requested runtime type");
    }

    IAssetFactory* Factory = FactoryIt->second.get();

    // Verify the factory handles this cooked payload type
    if (Factory->GetCookedPayloadType() != Info.CookedPayloadType)
    {
      return std::unexpected("Factory cooked type mismatch - asset has type " + Info.CookedPayloadType.ToString() + " but factory expects " +
                             Factory->GetCookedPayloadType().ToString());
    }

    // Load cooked payload
    auto PayloadResult = Reader->LoadCookedPayload(Info.Id);
    if (!PayloadResult.has_value())
    {
      return std::unexpected(PayloadResult.error());
    }

    // Create load context
    AssetLoadContext Context{.Cooked = *PayloadResult,
                             .Info = Info,
                             .LoadBulk = [Reader, Id = Info.Id](uint32_t Index) { return Reader->LoadBulkChunk(Id, Index); },
                             .GetBulkInfo = [Reader, Id = Info.Id](uint32_t Index) { return Reader->GetBulkChunkInfo(Id, Index); },
                             .Registry = &m_Impl->Registry};

    // Invoke factory
    return Factory->Load(Context);
  }

  std::expected<UniqueVoidPtr, std::string> AssetManager::LoadAnyById(AssetId Id, std::type_index RuntimeType)
  {
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
    auto FactoryIt = m_Impl->FactoriesByRuntimeType.find(RuntimeType);
    if (FactoryIt == m_Impl->FactoriesByRuntimeType.end())
    {
      return std::unexpected("No factory registered for requested runtime type");
    }

    IAssetFactory* Factory = FactoryIt->second.get();

    // Verify the factory handles this cooked payload type
    if (Factory->GetCookedPayloadType() != Info.CookedPayloadType)
    {
      return std::unexpected("Factory cooked type mismatch - asset has type " + Info.CookedPayloadType.ToString() + " but factory expects " +
                             Factory->GetCookedPayloadType().ToString());
    }

    // Load cooked payload
    auto PayloadResult = Reader->LoadCookedPayload(Id);
    if (!PayloadResult.has_value())
    {
      return std::unexpected(PayloadResult.error());
    }

    // Create load context
    AssetLoadContext Context{.Cooked = *PayloadResult,
                             .Info = Info,
                             .LoadBulk = [Reader, Id](uint32_t Index) { return Reader->LoadBulkChunk(Id, Index); },
                             .GetBulkInfo = [Reader, Id](uint32_t Index) { return Reader->GetBulkChunkInfo(Id, Index); },
                             .Registry = &m_Impl->Registry};

    // Invoke factory
    return Factory->Load(Context);
  }

  std::expected<AssetId, std::string> AssetManager::ResolveAssetId(const std::string& Name, std::type_index RuntimeType)
  {
    auto Result = FindAsset(Name);
    if (!Result.has_value())
    {
      return std::unexpected(Result.error());
    }
    return Result->Id;
  }

  size_t AssetManager::EstimateAssetSize(AssetId Id, std::type_index RuntimeType)
  {
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
    if (m_Impl->Pipeline)
    {
      m_Impl->Pipeline->RegisterImporter(std::move(Importer));
    }
  }

  void AssetManager::RegisterCooker(std::unique_ptr<IAssetCooker> Cooker)
  {
    if (m_Impl->Pipeline)
    {
      m_Impl->Pipeline->RegisterCooker(std::move(Cooker));
    }
  }

  std::expected<void, std::string> AssetManager::SaveRuntimeAssets()
  {
    if (!m_Impl->Pipeline)
    {
      return std::unexpected("Runtime pipeline not initialized");
    }
    return m_Impl->Pipeline->SaveAll();
  }

  uint32_t AssetManager::GetDirtyAssetCount() const
  {
    if (!m_Impl->Pipeline)
    {
      return 0;
    }
    return m_Impl->Pipeline->GetDirtyCount();
  }

  std::expected<AssetId, std::string> AssetManager::TryPipelineSource(const std::string& Name)
  {
    // Check if already pipelined this session
    if (m_Impl->Pipeline->HasAsset(Name))
    {
      return m_Impl->Pipeline->GetAssetId(Name);
    }

    // Resolve source path
    auto Resolved = m_Impl->SourceResolver->Resolve(Name);
    if (!Resolved)
    {
      return std::unexpected("Source not found: " + Name);
    }

    // Pipeline it
    auto Result = m_Impl->Pipeline->ProcessSource(Resolved->AbsolutePath, Resolved->LogicalName);
    if (!Result.has_value())
    {
      return std::unexpected(Result.error());
    }
    return Result->Id;
  }

  std::expected<UniqueVoidPtr, std::string> AssetManager::LoadFromRuntimePipeline(
      const std::string& Name, std::type_index RuntimeType)
  {
    const auto* Asset = m_Impl->Pipeline->GetCookedAsset(Name);
    if (!Asset)
    {
      return std::unexpected("Asset not in runtime pipeline: " + Name);
    }

    // Find factory for this runtime type
    auto FactoryIt = m_Impl->FactoriesByRuntimeType.find(RuntimeType);
    if (FactoryIt == m_Impl->FactoriesByRuntimeType.end())
    {
      return std::unexpected("No factory registered for requested runtime type");
    }

    IAssetFactory* Factory = FactoryIt->second.get();

    // Verify the factory handles this cooked payload type
    if (Factory->GetCookedPayloadType() != Asset->Cooked.PayloadType)
    {
      return std::unexpected("Factory cooked type mismatch - asset has type " + Asset->Cooked.PayloadType.ToString() +
                             " but factory expects " + Factory->GetCookedPayloadType().ToString());
    }

    // Build AssetInfo for the context
    AssetInfo Info;
    Info.Id = Asset->Id;
    Info.AssetKind = Asset->AssetKind;
    Info.CookedPayloadType = Asset->Cooked.PayloadType;
    Info.SchemaVersion = 0;
    Info.Name = Asset->LogicalName;
    Info.BulkChunkCount = static_cast<uint32_t>(Asset->Bulk.size());

    // Create load context with in-memory data
    const auto& BulkChunks = Asset->Bulk;
    AssetLoadContext Context{
        .Cooked = Asset->Cooked,
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
        .Registry = &m_Impl->Registry};

    return Factory->Load(Context);
  }

} // namespace SnAPI::AssetPipeline
