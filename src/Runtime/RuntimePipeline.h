#pragma once

#include "RuntimePipelineConfig.h"
#include "TypedPayload.h"
#include "IAssetCooker.h"
#include "Uuid.h"

#include <expected>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace SnAPI::AssetPipeline
{

class PayloadRegistry;
class IPipelineContext;
class PluginLoaderInternal;

class RuntimePipeline
{
  public:
    explicit RuntimePipeline(const RuntimePipelineConfig& Config);
    ~RuntimePipeline();

    std::expected<void, std::string> Initialize();

    struct PipelineResult
    {
        AssetId Id;
        std::string LogicalName;
    };

    // Pipeline a source file. Returns the cooked data stored in-memory.
    std::expected<PipelineResult, std::string> ProcessSource(const std::string& AbsolutePath, const std::string& LogicalName);

    // Write dirty assets to disk snpak
    std::expected<void, std::string> SaveAll();

    // Check if an asset has already been pipelined this session
    bool HasAsset(const std::string& LogicalName) const;

    // Get the asset ID for a pipelined asset
    std::expected<AssetId, std::string> GetAssetId(const std::string& LogicalName) const;

    uint32_t GetDirtyCount() const;

    // In-memory cooked asset (accessible to AssetManager for direct loading)
    struct CookedAsset
    {
        AssetId Id;
        std::string LogicalName;
        TypeId AssetKind;
        TypedPayload Cooked;
        std::vector<BulkChunk> Bulk;
        bool bDirty = true;
    };

    // Get a cooked asset by logical name (for direct loading by AssetManager)
    const CookedAsset* GetCookedAsset(const std::string& LogicalName) const;

    // Direct registration (no plugin DLL needed) - for testing and embedded use
    void RegisterImporter(std::unique_ptr<IAssetImporter> Importer);
    void RegisterCooker(std::unique_ptr<IAssetCooker> Cooker);

  private:
    std::expected<PipelineResult, std::string> DoProcessSource(const std::string& AbsolutePath, const std::string& LogicalName);

    RuntimePipelineConfig m_Config;
    std::unique_ptr<PayloadRegistry> m_Registry;
    std::unique_ptr<IPipelineContext> m_Context;
    std::unique_ptr<PluginLoaderInternal> m_PluginLoader;

    std::unordered_map<std::string, CookedAsset> m_CookedAssets;
    mutable std::mutex m_Mutex;

    std::unordered_map<std::string, std::shared_future<std::expected<PipelineResult, std::string>>> m_InFlight;
    std::mutex m_InFlightMutex;
};

} // namespace SnAPI::AssetPipeline
