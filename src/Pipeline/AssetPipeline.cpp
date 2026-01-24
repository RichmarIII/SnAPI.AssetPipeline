#include "AssetPipeline.h"
#include "AssetPackWriter.h"
#include "AssetPackReader.h"
#include "PayloadRegistry.h"
#include "IPipelineContext.h"
#include "IAssetImporter.h"
#include "IAssetCooker.h"
#include "IPluginRegistrar.h"

#include "Pack/SnPakFormat.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <filesystem>
#include <fstream>
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

  // Include PluginLoader implementation (defined in PluginLoader.cpp, but we need the class)
  class PluginLoader;

  // LoadedPlugin structure (must match PluginLoader.cpp)
  struct LoadedPlugin
  {
      std::string Name;
      std::string Version;
      std::string Path;

#ifdef _WIN32
      void* Handle = nullptr;
#else
      void* Handle = nullptr;
#endif

      std::vector<std::unique_ptr<IAssetImporter>> Importers;
      std::vector<std::unique_ptr<IAssetCooker>> Cookers;
      std::vector<std::unique_ptr<IPayloadSerializer>> Serializers;
  };

  // PluginLoader inline implementation for the engine
  class PluginLoaderInternal
  {
    public:
      ~PluginLoaderInternal()
      {
        UnloadAll();
      }

      bool LoadPlugin(const std::string& Path);
      void UnloadAll();

      void TransferSerializers(PayloadRegistry& Registry)
      {
        for (auto& Plugin : m_Plugins)
        {
          for (auto& Serializer : Plugin.Serializers)
          {
            Registry.Register(std::move(Serializer));
          }
          Plugin.Serializers.clear();
        }
      }

      const std::vector<LoadedPlugin>& GetPlugins() const
      {
        return m_Plugins;
      }

      std::vector<IAssetImporter*> GetAllImporters() const
      {
        std::vector<IAssetImporter*> Result;
        for (const auto& Plugin : m_Plugins)
        {
          for (const auto& Importer : Plugin.Importers)
          {
            Result.push_back(Importer.get());
          }
        }
        return Result;
      }

      std::vector<IAssetCooker*> GetAllCookers() const
      {
        std::vector<IAssetCooker*> Result;
        for (const auto& Plugin : m_Plugins)
        {
          for (const auto& Cooker : Plugin.Cookers)
          {
            Result.push_back(Cooker.get());
          }
        }
        return Result;
      }

      IAssetImporter* FindImporter(const SourceRef& Source) const
      {
        for (const auto& Plugin : m_Plugins)
        {
          for (const auto& Importer : Plugin.Importers)
          {
            if (Importer->CanImport(Source))
            {
              return Importer.get();
            }
          }
        }
        return nullptr;
      }

      IAssetCooker* FindCooker(TypeId AssetKind, TypeId IntermediateType) const
      {
        for (const auto& Plugin : m_Plugins)
        {
          for (const auto& Cooker : Plugin.Cookers)
          {
            if (Cooker->CanCook(AssetKind, IntermediateType))
            {
              return Cooker.get();
            }
          }
        }
        return nullptr;
      }

    private:
      std::vector<LoadedPlugin> m_Plugins;
  };

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
  };

  AssetPipelineEngine::AssetPipelineEngine() : m_Impl(std::make_unique<Impl>()) {}

  AssetPipelineEngine::~AssetPipelineEngine() = default;

  std::expected<void, std::string> AssetPipelineEngine::Initialize(const PipelineBuildConfig& Config) const
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

    // Validate config
    if (Config.OutputPackPath.empty())
    {
      return std::unexpected("OutputPackPath is required");
    }

    if (Config.SourceRoots.empty())
    {
      return std::unexpected("At least one SourceRoot is required");
    }

    // Verify source roots exist
    for (const auto& Root : Config.SourceRoots)
    {
      if (!std::filesystem::exists(Root))
      {
        return std::unexpected("Source root does not exist: " + Root);
      }
    }

    // Freeze registry after all plugins loaded
    m_Impl->Registry->Freeze();

    return {};
  }

  BuildResult AssetPipelineEngine::BuildAll() const
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

    // Set compression from config
    if (m_Impl->Config.BuildOptions.count("compression"))
    {
      const auto& Comp = m_Impl->Config.BuildOptions.at("compression");
      if (Comp == "none")
        Writer.SetCompression(EPackCompression::None);
      else if (Comp == "lz4")
        Writer.SetCompression(EPackCompression::LZ4);
      else
        Writer.SetCompression(EPackCompression::Zstd);
    }
    else
    {
      Writer.SetCompression(EPackCompression::Zstd);
    }

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

  BuildResult AssetPipelineEngine::BuildChanged() const
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
    Writer.SetCompression(EPackCompression::Zstd);

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

  BuildResult AssetPipelineEngine::BuildAsset(const std::string& SourcePath, const std::string& OutputPack, bool bAppend) const
  {
    return BuildAssets({SourcePath}, OutputPack, bAppend);
  }

  BuildResult AssetPipelineEngine::BuildAssets(const std::vector<std::string>& SourcePaths, const std::string& OutputPack, bool bAppend) const
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
    Writer.SetCompression(EPackCompression::Zstd);

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

  // PluginLoader implementation

  class PluginRegistrarImpl : public IPluginRegistrar
  {
    public:
      explicit PluginRegistrarImpl(LoadedPlugin& Plugin) : m_Plugin(Plugin) {}

      void RegisterImporter(std::unique_ptr<IAssetImporter> Importer) override
      {
        m_Plugin.Importers.push_back(std::move(Importer));
      }

      void RegisterCooker(std::unique_ptr<IAssetCooker> Cooker) override
      {
        m_Plugin.Cookers.push_back(std::move(Cooker));
      }

      void RegisterPayloadSerializer(std::unique_ptr<IPayloadSerializer> Serializer) override
      {
        m_Plugin.Serializers.push_back(std::move(Serializer));
      }

      void RegisterPluginInfo(const char* Name, const char* VersionString) override
      {
        m_Plugin.Name = Name ? Name : "";
        m_Plugin.Version = VersionString ? VersionString : "";
      }

    private:
      LoadedPlugin& m_Plugin;
  };

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
