#include "AssetPipeline.h"
#include "AssetPackWriter.h"
#include "AssetPackReader.h"
#include "PayloadRegistry.h"
#include "IPipelineContext.h"
#include "IAssetImporter.h"
#include "IAssetCooker.h"
#include "IPluginRegistrar.h"

#include "Pipeline/PluginLoaderInternal.h"
#include "Pack/SnPakFormat.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <filesystem>
#include <fstream>
#include <future>
#include <thread>
#include <mutex>
#include <atomic>
#include <queue>
#include <functional>
#include <unordered_set>

namespace SnAPI::AssetPipeline
{

  // Forward declarations
  std::unique_ptr<IPipelineContext> CreatePipelineContext(PayloadRegistry* Registry, const std::unordered_map<std::string, std::string>* Options);

  namespace
  {
    bool TryParseCompressionMode(const std::string& Mode, EPackCompression& Out)
    {
      if (Mode == "none")
      {
        Out = EPackCompression::None;
        return true;
      }
      if (Mode == "lz4")
      {
        Out = EPackCompression::LZ4;
        return true;
      }
      if (Mode == "lz4hc")
      {
        Out = EPackCompression::LZ4HC;
        return true;
      }
      if (Mode == "zstd")
      {
        Out = EPackCompression::Zstd;
        return true;
      }
      if (Mode == "zstdfast")
      {
        Out = EPackCompression::ZstdFast;
        return true;
      }
      return false;
    }

    bool TryParseCompressionLevel(const std::string& Level, EPackCompressionLevel& Out)
    {
      if (Level == "fast")
      {
        Out = EPackCompressionLevel::Fast;
        return true;
      }
      if (Level == "default")
      {
        Out = EPackCompressionLevel::Default;
        return true;
      }
      if (Level == "high")
      {
        Out = EPackCompressionLevel::High;
        return true;
      }
      if (Level == "max")
      {
        Out = EPackCompressionLevel::Max;
        return true;
      }
      return false;
    }

    void ApplyCompressionOptions(const PipelineBuildConfig& Config, AssetPackWriter& Writer)
    {
      Writer.SetCompression(Config.Compression);
      Writer.SetCompressionLevel(Config.CompressionLevel);

      auto It = Config.BuildOptions.find("compression");
      if (It != Config.BuildOptions.end())
      {
        EPackCompression Mode = Config.Compression;
        if (TryParseCompressionMode(It->second, Mode))
        {
          Writer.SetCompression(Mode);
        }
      }

      It = Config.BuildOptions.find("compression_level");
      if (It != Config.BuildOptions.end())
      {
        EPackCompressionLevel Level = Config.CompressionLevel;
        if (TryParseCompressionLevel(It->second, Level))
        {
          Writer.SetCompressionLevel(Level);
        }
      }
    }
  } // namespace

  // Incremental cache for tracking built assets
  class IncrementalCache
  {
    public:
      bool Open(const std::string& CachePath)
      {
        m_CachePath = CachePath;
        // Load existing cache from file
        std::ifstream File(CachePath, std::ios::binary);
        if (File.is_open())
        {
          uint32_t Count = 0;
          File.read(reinterpret_cast<char*>(&Count), sizeof(Count));
          for (uint32_t I = 0; I < Count && File.good(); ++I)
          {
            uint32_t UriLen = 0;
            File.read(reinterpret_cast<char*>(&UriLen), sizeof(UriLen));
            if (UriLen > 4096)
              break; // Sanity check

            std::string Uri(UriLen, '\0');
            File.read(Uri.data(), UriLen);

            uint64_t Hash = 0;
            File.read(reinterpret_cast<char*>(&Hash), sizeof(Hash));

            m_Cache[Uri] = Hash;
          }
        }
        return true;
      }

      void Save()
      {
        std::ofstream File(m_CachePath, std::ios::binary);
        if (File.is_open())
        {
          uint32_t Count = static_cast<uint32_t>(m_Cache.size());
          File.write(reinterpret_cast<const char*>(&Count), sizeof(Count));
          for (const auto& [Uri, Hash] : m_Cache)
          {
            uint32_t UriLen = static_cast<uint32_t>(Uri.size());
            File.write(reinterpret_cast<const char*>(&UriLen), sizeof(UriLen));
            File.write(Uri.data(), UriLen);
            File.write(reinterpret_cast<const char*>(&Hash), sizeof(Hash));
          }
        }
      }

      bool IsUpToDate(const std::string& Uri, uint64_t ContentHash) const
      {
        auto It = m_Cache.find(Uri);
        return It != m_Cache.end() && It->second == ContentHash;
      }

      void Update(const std::string& Uri, uint64_t ContentHash)
      {
        m_Cache[Uri] = ContentHash;
      }

      void Remove(const std::string& Uri)
      {
        m_Cache.erase(Uri);
      }

    private:
      std::string m_CachePath;
      std::unordered_map<std::string, uint64_t> m_Cache;
  };

  struct AssetPipelineEngine::Impl
  {
      PipelineBuildConfig Config;

      std::unique_ptr<PayloadRegistry> Registry;
      std::unique_ptr<IPipelineContext> Context;
      std::unique_ptr<PluginLoaderInternal> Loader;
      std::unique_ptr<IncrementalCache> Cache;

      std::vector<PluginInfo> PluginInfos;
      std::vector<ImporterInfo> ImporterInfos;
      std::vector<CookerInfo> CookerInfos;

      // In-memory cooked assets (on-demand processing)
      std::unordered_map<std::string, CookedAsset> CookedAssets;
      mutable std::mutex AssetsMutex;

      // In-flight deduplication
      std::unordered_map<std::string, std::shared_future<std::expected<PipelineResult, std::string>>> InFlight;
      std::mutex InFlightMutex;

      std::mutex ErrorMutex;
      std::vector<std::string> Errors;
      std::vector<std::string> Warnings;

      void LogError(const std::string& Msg)
      {
        std::lock_guard Lock(ErrorMutex);
        Errors.push_back(Msg);
      }

      void LogWarning(const std::string& Msg)
      {
        std::lock_guard Lock(ErrorMutex);
        Warnings.push_back(Msg);
      }

      void ClearLogs()
      {
        std::lock_guard Lock(ErrorMutex);
        Errors.clear();
        Warnings.clear();
      }

      // Compute content hash for a file
      uint64_t ComputeFileHash(const std::string& Path)
      {
        std::ifstream File(Path, std::ios::binary | std::ios::ate);
        if (!File.is_open())
        {
          return 0;
        }

        auto Size = File.tellg();
        if (Size <= 0)
        {
          return 0;
        }

        std::vector<uint8_t> Data(Size);
        File.seekg(0);
        File.read(reinterpret_cast<char*>(Data.data()), Size);
        return XXH3_64bits(Data.data(), Data.size());
      }

      // Process a single source file through the pipeline
      // Returns: number of assets successfully cooked (0 on failure)
      uint32_t ProcessSource(const SourceRef& Source, AssetPackWriter& Writer)
      {
        // Find importer
        IAssetImporter* Importer = Loader->FindImporter(Source);
        if (!Importer)
        {
          LogWarning("No importer found for: " + Source.Uri);
          return 0;
        }

        // Import
        std::vector<ImportedItem> Items;
        if (!Importer->Import(Source, Items, *Context))
        {
          LogError("Import failed for: " + Source.Uri);
          return 0;
        }

        if (Items.empty())
        {
          LogWarning("Import produced no items for: " + Source.Uri);
          return 0;
        }

        uint32_t SuccessCount = 0;

        // Cook each imported item
        for (auto& Item : Items)
        {
          // Find cooker
          IAssetCooker* Cooker = Loader->FindCooker(Item.AssetKind, Item.Intermediate.PayloadType);
          if (!Cooker)
          {
            LogWarning("No cooker found for asset: " + Item.LogicalName + " (Kind: " + Item.AssetKind.ToString() +
                       ", Type: " + Item.Intermediate.PayloadType.ToString() + ")");
            continue;
          }

          // Build cook request
          CookRequest Req;
          Req.Id = Item.Id;
          Req.LogicalName = Item.LogicalName;
          Req.AssetKind = Item.AssetKind;
          Req.VariantKey = Item.VariantKey;
          Req.Intermediate = std::move(Item.Intermediate);
          Req.Dependencies = std::move(Item.Dependencies);
          Req.BuildOptions = Config.BuildOptions;

          // Cook
          CookResult Result;
          if (!Cooker->Cook(Req, Result, *Context))
          {
            LogError("Cook failed for asset: " + Req.LogicalName);
            continue;
          }

          // Add to pack writer
          AssetPackEntry Entry;
          Entry.Id = Req.Id;
          Entry.AssetKind = Req.AssetKind;
          Entry.Name = Req.LogicalName;
          Entry.VariantKey = Req.VariantKey;
          Entry.Cooked = std::move(Result.Cooked);
          Entry.Bulk = std::move(Result.Bulk);

          Writer.AddAsset(std::move(Entry));
          ++SuccessCount;
        }

        return SuccessCount;
      }

      // Scan source roots for all files
      std::vector<SourceRef> ScanSources()
      {
        std::vector<SourceRef> Sources;

        for (const auto& Root : Config.SourceRoots)
        {
          try
          {
            for (const auto& Entry : std::filesystem::recursive_directory_iterator(Root))
            {
              if (Entry.is_regular_file())
              {
                SourceRef Ref;
                Ref.Uri = Entry.path().string();
                Ref.ContentHash = ComputeFileHash(Ref.Uri);
                Sources.push_back(std::move(Ref));
              }
            }
          }
          catch (const std::exception& E)
          {
            LogWarning(std::string("Failed to scan source root: ") + Root + " - " + E.what());
          }
        }

        return Sources;
      }

      // On-demand: process a source file and store in memory
      std::expected<PipelineResult, std::string> ProcessSourceToMemory(
          const std::string& AbsolutePath, const std::string& LogicalName)
      {
        // Check if already processed
        {
          std::lock_guard Lock(AssetsMutex);
          auto It = CookedAssets.find(LogicalName);
          if (It != CookedAssets.end())
          {
            return PipelineResult{It->second.Id, It->second.LogicalName};
          }
        }

        // Check for in-flight processing (deduplication)
        std::shared_future<std::expected<PipelineResult, std::string>> Future;
        {
          std::lock_guard Lock(InFlightMutex);
          auto It = InFlight.find(LogicalName);
          if (It != InFlight.end())
          {
            Future = It->second;
          }
          else
          {
            std::promise<std::expected<PipelineResult, std::string>> Promise;
            Future = Promise.get_future().share();
            InFlight[LogicalName] = Future;

            auto Result = DoProcessSourceToMemory(AbsolutePath, LogicalName);
            Promise.set_value(Result);

            InFlight.erase(LogicalName);
            return Result;
          }
        }

        return Future.get();
      }

      // Actually process one source file to in-memory storage
      std::expected<PipelineResult, std::string> DoProcessSourceToMemory(
          const std::string& AbsolutePath, const std::string& LogicalName)
      {
        // Read file and compute hash
        std::ifstream File(AbsolutePath, std::ios::binary | std::ios::ate);
        if (!File.is_open())
        {
          return std::unexpected("Cannot open source file: " + AbsolutePath);
        }

        auto FileSize = File.tellg();
        if (FileSize <= 0)
        {
          return std::unexpected("Source file is empty: " + AbsolutePath);
        }

        std::vector<uint8_t> FileData(static_cast<size_t>(FileSize));
        File.seekg(0);
        File.read(reinterpret_cast<char*>(FileData.data()), FileSize);
        File.close();

        uint64_t ContentHash = XXH3_64bits(FileData.data(), FileData.size());

        // Create source ref
        SourceRef Source;
        Source.Uri = AbsolutePath;
        Source.ContentHash = ContentHash;

        // Find importer
        IAssetImporter* Importer = Loader->FindImporter(Source);
        if (!Importer)
        {
          return std::unexpected("No importer found for: " + AbsolutePath);
        }

        // Import
        std::vector<ImportedItem> Items;
        if (!Importer->Import(Source, Items, *Context))
        {
          return std::unexpected("Import failed for: " + AbsolutePath);
        }

        if (Items.empty())
        {
          return std::unexpected("Import produced no items for: " + AbsolutePath);
        }

        PipelineResult FinalResult;

        // Cook each imported item
        for (auto& Item : Items)
        {
          Item.LogicalName = LogicalName;

          if (Config.bDeterministicAssetIds)
          {
            Item.Id = Context->MakeDeterministicAssetId(Item.LogicalName, Item.VariantKey);
          }

          IAssetCooker* Cooker = Loader->FindCooker(Item.AssetKind, Item.Intermediate.PayloadType);
          if (!Cooker)
          {
            return std::unexpected("No cooker found for asset: " + Item.LogicalName +
                                   " (Kind: " + Item.AssetKind.ToString() + ")");
          }

          CookRequest Req;
          Req.Id = Item.Id;
          Req.LogicalName = Item.LogicalName;
          Req.AssetKind = Item.AssetKind;
          Req.VariantKey = Item.VariantKey;
          Req.Intermediate = std::move(Item.Intermediate);
          Req.Dependencies = std::move(Item.Dependencies);
          Req.BuildOptions = Config.BuildOptions;

          CookResult Result;
          if (!Cooker->Cook(Req, Result, *Context))
          {
            return std::unexpected("Cook failed for asset: " + Req.LogicalName);
          }

          // Store in memory
          CookedAsset Asset;
          Asset.Id = Req.Id;
          Asset.LogicalName = Req.LogicalName;
          Asset.AssetKind = Req.AssetKind;
          Asset.Cooked = std::move(Result.Cooked);
          Asset.Bulk = std::move(Result.Bulk);
          Asset.bDirty = true;

          FinalResult.Id = Asset.Id;
          FinalResult.LogicalName = Asset.LogicalName;

          {
            std::lock_guard Lock(AssetsMutex);
            CookedAssets[Req.LogicalName] = std::move(Asset);
          }
        }

        return FinalResult;
      }
  };

  AssetPipelineEngine::AssetPipelineEngine() : m_Impl(std::make_unique<Impl>()) {}

  AssetPipelineEngine::~AssetPipelineEngine() = default;

  std::expected<void, std::string> AssetPipelineEngine::Initialize(const PipelineBuildConfig& Config)
  {
    m_Impl->Config = Config;

    // Create registry
    m_Impl->Registry = std::make_unique<PayloadRegistry>();

    // Create pipeline context
    m_Impl->Context = CreatePipelineContext(m_Impl->Registry.get(), &Config.BuildOptions);

    // Create plugin loader
    m_Impl->Loader = std::make_unique<PluginLoaderInternal>();

    // Load plugins
    for (const auto& PluginPath : Config.PluginPaths)
    {
      if (m_Impl->Loader->LoadPlugin(PluginPath))
      {
        // Get plugin info for reporting
        const auto& Plugins = m_Impl->Loader->GetPlugins();
        if (!Plugins.empty())
        {
          const auto& LastPlugin = Plugins.back();
          PluginInfo Info;
          Info.Name = LastPlugin.Name;
          Info.Version = LastPlugin.Version;
          Info.Path = LastPlugin.Path;
          m_Impl->PluginInfos.push_back(std::move(Info));
        }
      }
      else
      {
        m_Impl->LogWarning("Failed to load plugin: " + PluginPath);
      }
    }

    // Transfer serializers to registry
    m_Impl->Loader->TransferSerializers(*m_Impl->Registry);

    // Build importer/cooker info lists
    for (auto* Importer : m_Impl->Loader->GetAllImporters())
    {
      ImporterInfo Info;
      Info.Name = Importer->GetName();
      m_Impl->ImporterInfos.push_back(std::move(Info));
    }

    for (auto* Cooker : m_Impl->Loader->GetAllCookers())
    {
      CookerInfo Info;
      Info.Name = Cooker->GetName();
      m_Impl->CookerInfos.push_back(std::move(Info));
    }

    // Initialize incremental cache
    m_Impl->Cache = std::make_unique<IncrementalCache>();
    if (!Config.CacheDatabasePath.empty())
    {
      m_Impl->Cache->Open(Config.CacheDatabasePath);
    }

    // Verify source roots exist (if specified)
    for (const auto& Root : Config.SourceRoots)
    {
      if (!std::filesystem::exists(Root))
      {
        return std::unexpected("Source root does not exist: " + Root);
      }
    }

    return {};
  }

  BuildResult AssetPipelineEngine::BuildAll()
  {
    BuildResult Result;
    Result.bSuccess = true;

    m_Impl->ClearLogs();

    // Scan all source files
    std::vector<SourceRef> Sources = m_Impl->ScanSources();

    if (Sources.empty())
    {
      m_Impl->LogWarning("No source files found");
      Result.Warnings = m_Impl->Warnings;
      return Result;
    }

    // Create pack writer
    AssetPackWriter Writer;

    ApplyCompressionOptions(m_Impl->Config, Writer);

    // Process each source
    for (const auto& Source : Sources)
    {
      uint32_t Count = m_Impl->ProcessSource(Source, Writer);
      if (Count > 0)
      {
        Result.AssetsBuilt += Count;
        // Update cache
        m_Impl->Cache->Update(Source.Uri, Source.ContentHash);
      }
      else
      {
        ++Result.AssetsFailed;
      }
    }

    // Write pack file
    auto WriteResult = Writer.Write(m_Impl->Config.OutputPackPath);
    if (!WriteResult.has_value())
    {
      m_Impl->LogError("Failed to write pack: " + WriteResult.error());
      Result.bSuccess = false;
    }

    // Save cache
    m_Impl->Cache->Save();

    Result.Errors = m_Impl->Errors;
    Result.Warnings = m_Impl->Warnings;

    if (!Result.Errors.empty())
    {
      Result.bSuccess = false;
    }

    return Result;
  }

  BuildResult AssetPipelineEngine::BuildChanged()
  {
    BuildResult Result;
    Result.bSuccess = true;

    m_Impl->ClearLogs();

    // Scan all source files
    std::vector<SourceRef> Sources = m_Impl->ScanSources();

    if (Sources.empty())
    {
      m_Impl->LogWarning("No source files found");
      Result.Warnings = m_Impl->Warnings;
      return Result;
    }

    // Filter to only changed sources
    std::vector<SourceRef> ChangedSources;
    for (const auto& Source : Sources)
    {
      if (!m_Impl->Cache->IsUpToDate(Source.Uri, Source.ContentHash))
      {
        ChangedSources.push_back(Source);
      }
      else
      {
        ++Result.AssetsSkipped;
      }
    }

    if (ChangedSources.empty())
    {
      Result.Warnings = m_Impl->Warnings;
      return Result;
    }

    // Use append mode if pack exists
    bool bAppend = std::filesystem::exists(m_Impl->Config.OutputPackPath);

    AssetPackWriter Writer;
    ApplyCompressionOptions(m_Impl->Config, Writer);

    // If appending, load existing pack first
    if (bAppend)
    {
      AssetPackReader Reader;
      auto OpenResult = Reader.Open(m_Impl->Config.OutputPackPath);
      if (OpenResult.has_value())
      {
        // Copy existing assets (that aren't being rebuilt)
        std::unordered_set<std::string> ChangedUris;
        for (const auto& S : ChangedSources)
        {
          ChangedUris.insert(S.Uri);
        }

        for (uint32_t I = 0; I < Reader.GetAssetCount(); ++I)
        {
          auto Info = Reader.GetAssetInfo(I);
          if (Info.has_value())
          {
            // We'd need a way to map assets back to source URIs
            // For now, just rebuild everything in append mode
            // A proper implementation would track source->asset mapping
          }
        }
      }
    }

    // Process changed sources
    for (const auto& Source : ChangedSources)
    {
      uint32_t Count = m_Impl->ProcessSource(Source, Writer);
      if (Count > 0)
      {
        Result.AssetsBuilt += Count;
        m_Impl->Cache->Update(Source.Uri, Source.ContentHash);
      }
      else
      {
        ++Result.AssetsFailed;
      }
    }

    // Write pack file
    auto WriteResult = bAppend ? Writer.AppendUpdate(m_Impl->Config.OutputPackPath) : Writer.Write(m_Impl->Config.OutputPackPath);

    if (!WriteResult.has_value())
    {
      m_Impl->LogError("Failed to write pack: " + WriteResult.error());
      Result.bSuccess = false;
    }

    // Save cache
    m_Impl->Cache->Save();

    Result.Errors = m_Impl->Errors;
    Result.Warnings = m_Impl->Warnings;

    if (!Result.Errors.empty())
    {
      Result.bSuccess = false;
    }

    return Result;
  }

  BuildResult AssetPipelineEngine::BuildAsset(const std::string& SourcePath, const std::string& OutputPack, bool bAppend)
  {
    return BuildAssets({SourcePath}, OutputPack, bAppend);
  }

  BuildResult AssetPipelineEngine::BuildAssets(const std::vector<std::string>& SourcePaths, const std::string& OutputPack, bool bAppend)
  {
    BuildResult Result;
    Result.bSuccess = true;

    m_Impl->ClearLogs();

    if (SourcePaths.empty())
    {
      m_Impl->LogWarning("No source paths provided");
      Result.Warnings = m_Impl->Warnings;
      return Result;
    }

    // Determine output pack path
    std::string PackPath = OutputPack.empty() ? m_Impl->Config.OutputPackPath : OutputPack;
    if (PackPath.empty())
    {
      m_Impl->LogError("No output pack path specified");
      Result.bSuccess = false;
      Result.Errors = m_Impl->Errors;
      return Result;
    }

    // Build source refs
    std::vector<SourceRef> Sources;
    for (const auto& Path : SourcePaths)
    {
      if (!std::filesystem::exists(Path))
      {
        m_Impl->LogError("Source file not found: " + Path);
        ++Result.AssetsFailed;
        continue;
      }

      SourceRef Ref;
      Ref.Uri = Path;
      Ref.ContentHash = m_Impl->ComputeFileHash(Path);
      Sources.push_back(std::move(Ref));
    }

    if (Sources.empty())
    {
      Result.bSuccess = false;
      Result.Errors = m_Impl->Errors;
      Result.Warnings = m_Impl->Warnings;
      return Result;
    }

    // Create writer
    AssetPackWriter Writer;
    ApplyCompressionOptions(m_Impl->Config, Writer);

    // Process each source
    for (const auto& Source : Sources)
    {
      uint32_t Count = m_Impl->ProcessSource(Source, Writer);
      if (Count > 0)
      {
        Result.AssetsBuilt += Count;
        m_Impl->Cache->Update(Source.Uri, Source.ContentHash);
      }
      else
      {
        ++Result.AssetsFailed;
      }
    }

    // Write or append to pack
    std::expected<void, std::string> WriteResult;
    if (bAppend && std::filesystem::exists(PackPath))
    {
      WriteResult = Writer.AppendUpdate(PackPath);
    }
    else
    {
      WriteResult = Writer.Write(PackPath);
    }

    if (!WriteResult.has_value())
    {
      m_Impl->LogError("Failed to write pack: " + WriteResult.error());
      Result.bSuccess = false;
    }

    // Save cache
    m_Impl->Cache->Save();

    Result.Errors = m_Impl->Errors;
    Result.Warnings = m_Impl->Warnings;

    if (!Result.Errors.empty())
    {
      Result.bSuccess = false;
    }

    return Result;
  }

  std::vector<PluginInfo> AssetPipelineEngine::GetPlugins() const
  {
    return m_Impl->PluginInfos;
  }

  std::vector<ImporterInfo> AssetPipelineEngine::GetImporters() const
  {
    return m_Impl->ImporterInfos;
  }

  std::vector<CookerInfo> AssetPipelineEngine::GetCookers() const
  {
    return m_Impl->CookerInfos;
  }

  // ========== On-Demand Processing ==========

  std::expected<PipelineResult, std::string> AssetPipelineEngine::ProcessSource(
      const std::string& AbsolutePath, const std::string& LogicalName)
  {
    return m_Impl->ProcessSourceToMemory(AbsolutePath, LogicalName);
  }

  // ========== In-Memory Access ==========

  bool AssetPipelineEngine::HasAsset(const std::string& LogicalName) const
  {
    std::lock_guard Lock(m_Impl->AssetsMutex);
    return m_Impl->CookedAssets.contains(LogicalName);
  }

  std::expected<AssetId, std::string> AssetPipelineEngine::GetAssetId(const std::string& LogicalName) const
  {
    std::lock_guard Lock(m_Impl->AssetsMutex);
    auto It = m_Impl->CookedAssets.find(LogicalName);
    if (It == m_Impl->CookedAssets.end())
    {
      return std::unexpected("Asset not found: " + LogicalName);
    }
    return It->second.Id;
  }

  std::expected<std::reference_wrapper<const CookedAsset>, std::string> AssetPipelineEngine::GetCookedAsset(
      const std::string& LogicalName) const
  {
    std::lock_guard Lock(m_Impl->AssetsMutex);
    auto It = m_Impl->CookedAssets.find(LogicalName);
    if (It == m_Impl->CookedAssets.end())
    {
      return std::unexpected("Asset not found: " + LogicalName);
    }
    return std::cref(It->second);
  }

  uint32_t AssetPipelineEngine::GetDirtyCount() const
  {
    std::lock_guard Lock(m_Impl->AssetsMutex);
    uint32_t Count = 0;
    for (const auto& [Name, Asset] : m_Impl->CookedAssets)
    {
      if (Asset.bDirty)
      {
        ++Count;
      }
    }
    return Count;
  }

  // ========== Persistence ==========

  std::expected<void, std::string> AssetPipelineEngine::SaveAll()
  {
    std::lock_guard Lock(m_Impl->AssetsMutex);

    uint32_t DirtyCount = 0;
    for (const auto& [Name, Asset] : m_Impl->CookedAssets)
    {
      if (Asset.bDirty)
      {
        ++DirtyCount;
      }
    }

    if (DirtyCount == 0)
    {
      return {};
    }

    if (m_Impl->Config.OutputPackPath.empty())
    {
      return std::unexpected("No OutputPackPath configured - cannot save");
    }

    // Ensure parent directory exists
    std::filesystem::path OutputPath(m_Impl->Config.OutputPackPath);
    if (OutputPath.has_parent_path())
    {
      std::filesystem::create_directories(OutputPath.parent_path());
    }

    // Create writer with configured compression
    AssetPackWriter Writer;
    ApplyCompressionOptions(m_Impl->Config, Writer);

    // Add all dirty assets
    for (auto& [Name, Asset] : m_Impl->CookedAssets)
    {
      if (!Asset.bDirty)
      {
        continue;
      }

      AssetPackEntry Entry;
      Entry.Id = Asset.Id;
      Entry.AssetKind = Asset.AssetKind;
      Entry.Name = Asset.LogicalName;
      Entry.Cooked = Asset.Cooked;
      Entry.Bulk = Asset.Bulk;

      Writer.AddAsset(std::move(Entry));
    }

    // Write or append
    std::expected<void, std::string> WriteResult;
    if (std::filesystem::exists(m_Impl->Config.OutputPackPath))
    {
      WriteResult = Writer.AppendUpdate(m_Impl->Config.OutputPackPath);
    }
    else
    {
      WriteResult = Writer.Write(m_Impl->Config.OutputPackPath);
    }

    if (!WriteResult.has_value())
    {
      return std::unexpected("Failed to write pack: " + WriteResult.error());
    }

    // Mark all as clean
    for (auto& [Name, Asset] : m_Impl->CookedAssets)
    {
      Asset.bDirty = false;
    }

    return {};
  }

  // ========== Inline Registration ==========

  void AssetPipelineEngine::RegisterImporter(std::unique_ptr<IAssetImporter> Importer)
  {
    m_Impl->Loader->RegisterImporter(std::move(Importer));
  }

  void AssetPipelineEngine::RegisterCooker(std::unique_ptr<IAssetCooker> Cooker)
  {
    m_Impl->Loader->RegisterCooker(std::move(Cooker));
  }

  void AssetPipelineEngine::RegisterSerializer(std::unique_ptr<IPayloadSerializer> Serializer)
  {
    m_Impl->Loader->RegisterSerializer(std::move(Serializer));
    m_Impl->Loader->TransferSerializers(*m_Impl->Registry);
  }

  // ========== Registry Access ==========

  PayloadRegistry& AssetPipelineEngine::GetRegistry()
  {
    return *m_Impl->Registry;
  }

  const PayloadRegistry& AssetPipelineEngine::GetRegistry() const
  {
    return *m_Impl->Registry;
  }

  bool PluginLoaderInternal::LoadPlugin(const std::string& Path)
  {
    LoadedPlugin Plugin;
    Plugin.Path = Path;

#ifdef _WIN32
    Plugin.Handle = LoadLibraryA(Path.c_str());
    if (!Plugin.Handle)
    {
      return false;
    }

    auto EntryFunc = reinterpret_cast<PluginRegisterFunc>(GetProcAddress(static_cast<HMODULE>(Plugin.Handle), SNAPI_PLUGIN_ENTRY_NAME));
#else
    Plugin.Handle = dlopen(Path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!Plugin.Handle)
    {
      return false;
    }

    auto EntryFunc = reinterpret_cast<PluginRegisterFunc>(dlsym(Plugin.Handle, SNAPI_PLUGIN_ENTRY_NAME));
#endif

    if (!EntryFunc)
    {
#ifdef _WIN32
      FreeLibrary(static_cast<HMODULE>(Plugin.Handle));
#else
      dlclose(Plugin.Handle);
#endif
      return false;
    }

    // Call plugin registration
    PluginRegistrarImpl Registrar(Plugin);
    EntryFunc(Registrar);

    m_Plugins.push_back(std::move(Plugin));
    return true;
  }

  void PluginLoaderInternal::UnloadAll()
  {
    for (auto& Plugin : m_Plugins)
    {
      Plugin.Importers.clear();
      Plugin.Cookers.clear();
      Plugin.Serializers.clear();

      if (Plugin.Handle)
      {
#ifdef _WIN32
        FreeLibrary(static_cast<HMODULE>(Plugin.Handle));
#else
        dlclose(Plugin.Handle);
#endif
        Plugin.Handle = nullptr;
      }
    }
    m_Plugins.clear();
  }

} // namespace SnAPI::AssetPipeline
