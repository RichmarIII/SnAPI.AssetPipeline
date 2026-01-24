#include "Runtime/RuntimePipeline.h"
#include "Pipeline/PluginLoaderInternal.h"
#include "AssetPackWriter.h"
#include "PayloadRegistry.h"
#include "IPipelineContext.h"
#include "IAssetImporter.h"
#include "IAssetCooker.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <filesystem>
#include <fstream>

namespace SnAPI::AssetPipeline
{

  // Forward declaration (defined in PipelineContext.cpp)
  std::unique_ptr<IPipelineContext> CreatePipelineContext(PayloadRegistry* Registry, const std::unordered_map<std::string, std::string>* Options);

  RuntimePipeline::RuntimePipeline(const RuntimePipelineConfig& Config) : m_Config(Config) {}

  RuntimePipeline::~RuntimePipeline() = default;

  std::expected<void, std::string> RuntimePipeline::Initialize()
  {
    m_Registry = std::make_unique<PayloadRegistry>();
    m_Context = CreatePipelineContext(m_Registry.get(), &m_Config.BuildOptions);
    m_PluginLoader = std::make_unique<PluginLoaderInternal>();

    for (const auto& PluginPath : m_Config.PluginPaths)
    {
      if (!m_PluginLoader->LoadPlugin(PluginPath))
      {
        return std::unexpected("Failed to load plugin: " + PluginPath);
      }
    }

    m_PluginLoader->TransferSerializers(*m_Registry);
    return {};
  }

  std::expected<RuntimePipeline::PipelineResult, std::string> RuntimePipeline::ProcessSource(
      const std::string& AbsolutePath, const std::string& LogicalName)
  {
    // Check if already processed
    {
      std::lock_guard Lock(m_Mutex);
      auto It = m_CookedAssets.find(LogicalName);
      if (It != m_CookedAssets.end())
      {
        return PipelineResult{It->second.Id, It->second.LogicalName};
      }
    }

    // Check for in-flight processing (deduplication)
    std::shared_future<std::expected<PipelineResult, std::string>> Future;
    bool bIsFirstRequester = false;
    {
      std::lock_guard Lock(m_InFlightMutex);
      auto It = m_InFlight.find(LogicalName);
      if (It != m_InFlight.end())
      {
        Future = It->second;
      }
      else
      {
        bIsFirstRequester = true;
        std::promise<std::expected<PipelineResult, std::string>> Promise;
        Future = Promise.get_future().share();
        m_InFlight[LogicalName] = Future;

        // Actually do the work inline and set the promise
        auto Result = DoProcessSource(AbsolutePath, LogicalName);
        Promise.set_value(Result);

        m_InFlight.erase(LogicalName);
        return Result;
      }
    }

    // Wait on the existing future
    return Future.get();
  }

  std::expected<RuntimePipeline::PipelineResult, std::string> RuntimePipeline::DoProcessSource(
      const std::string& AbsolutePath, const std::string& LogicalName)
  {
    // Compute file hash
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
    IAssetImporter* Importer = m_PluginLoader->FindImporter(Source);
    if (!Importer)
    {
      return std::unexpected("No importer found for: " + AbsolutePath);
    }

    // Import
    std::vector<ImportedItem> Items;
    if (!Importer->Import(Source, Items, *m_Context))
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
      // Override logical name with what the user queries by
      Item.LogicalName = LogicalName;

      // Generate deterministic ID if configured
      if (m_Config.bDeterministicAssetIds)
      {
        Item.Id = m_Context->MakeDeterministicAssetId(Item.LogicalName, Item.VariantKey);
      }

      // Find cooker
      IAssetCooker* Cooker = m_PluginLoader->FindCooker(Item.AssetKind, Item.Intermediate.PayloadType);
      if (!Cooker)
      {
        return std::unexpected("No cooker found for asset: " + Item.LogicalName +
                               " (Kind: " + Item.AssetKind.ToString() + ")");
      }

      // Build cook request
      CookRequest Req;
      Req.Id = Item.Id;
      Req.LogicalName = Item.LogicalName;
      Req.AssetKind = Item.AssetKind;
      Req.VariantKey = Item.VariantKey;
      Req.Intermediate = std::move(Item.Intermediate);
      Req.Dependencies = std::move(Item.Dependencies);
      Req.BuildOptions = m_Config.BuildOptions;

      // Cook
      CookResult Result;
      if (!Cooker->Cook(Req, Result, *m_Context))
      {
        return std::unexpected("Cook failed for asset: " + Req.LogicalName);
      }

      // Store in-memory
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
        std::lock_guard Lock(m_Mutex);
        m_CookedAssets[Req.LogicalName] = std::move(Asset);
      }
    }

    return FinalResult;
  }

  std::expected<void, std::string> RuntimePipeline::SaveAll()
  {
    std::lock_guard Lock(m_Mutex);

    uint32_t DirtyCount = 0;
    for (const auto& [Name, Asset] : m_CookedAssets)
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

    // Determine output path
    std::string OutputPath;
    if (!m_Config.OutputDirectory.empty())
    {
      std::filesystem::create_directories(m_Config.OutputDirectory);
      OutputPath = (std::filesystem::path(m_Config.OutputDirectory) / m_Config.RuntimePackName).string();
    }
    else
    {
      OutputPath = m_Config.RuntimePackName;
    }

    // Create writer
    AssetPackWriter Writer;
    switch (m_Config.CompressionMode)
    {
    case PipelineBuildConfig::ECompressionMode::None:
      Writer.SetCompression(EPackCompression::None);
      break;
    case PipelineBuildConfig::ECompressionMode::LZ4:
      Writer.SetCompression(EPackCompression::LZ4);
      break;
    case PipelineBuildConfig::ECompressionMode::Zstd:
    case PipelineBuildConfig::ECompressionMode::ZstdMax:
      Writer.SetCompression(EPackCompression::Zstd);
      break;
    }

    // Add all dirty assets
    for (auto& [Name, Asset] : m_CookedAssets)
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
    if (std::filesystem::exists(OutputPath))
    {
      WriteResult = Writer.AppendUpdate(OutputPath);
    }
    else
    {
      WriteResult = Writer.Write(OutputPath);
    }

    if (!WriteResult.has_value())
    {
      return std::unexpected("Failed to write runtime pack: " + WriteResult.error());
    }

    // Mark all as clean
    for (auto& [Name, Asset] : m_CookedAssets)
    {
      Asset.bDirty = false;
    }

    return {};
  }

  bool RuntimePipeline::HasAsset(const std::string& LogicalName) const
  {
    std::lock_guard Lock(m_Mutex);
    return m_CookedAssets.contains(LogicalName);
  }

  std::expected<AssetId, std::string> RuntimePipeline::GetAssetId(const std::string& LogicalName) const
  {
    std::lock_guard Lock(m_Mutex);
    auto It = m_CookedAssets.find(LogicalName);
    if (It == m_CookedAssets.end())
    {
      return std::unexpected("Asset not found in runtime pipeline: " + LogicalName);
    }
    return It->second.Id;
  }

  uint32_t RuntimePipeline::GetDirtyCount() const
  {
    std::lock_guard Lock(m_Mutex);
    uint32_t Count = 0;
    for (const auto& [Name, Asset] : m_CookedAssets)
    {
      if (Asset.bDirty)
      {
        ++Count;
      }
    }
    return Count;
  }

  const RuntimePipeline::CookedAsset* RuntimePipeline::GetCookedAsset(const std::string& LogicalName) const
  {
    std::lock_guard Lock(m_Mutex);
    auto It = m_CookedAssets.find(LogicalName);
    if (It == m_CookedAssets.end())
    {
      return nullptr;
    }
    return &It->second;
  }

  void RuntimePipeline::RegisterImporter(std::unique_ptr<IAssetImporter> Importer)
  {
    m_PluginLoader->RegisterImporter(std::move(Importer));
  }

  void RuntimePipeline::RegisterCooker(std::unique_ptr<IAssetCooker> Cooker)
  {
    m_PluginLoader->RegisterCooker(std::move(Cooker));
  }

} // namespace SnAPI::AssetPipeline
