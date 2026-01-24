#pragma once

#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Export.h"
#include "Uuid.h"
#include "TypedPayload.h"
#include "IAssetCooker.h"
#include "PipelineBuildConfig.h"

namespace SnAPI::AssetPipeline
{

class IAssetImporter;
class IAssetCooker;
class IPayloadSerializer;
class PayloadRegistry;

struct SNAPI_ASSETPIPELINE_API BuildResult
{
    bool bSuccess = false;
    uint32_t AssetsBuilt = 0;
    uint32_t AssetsSkipped = 0;
    uint32_t AssetsFailed = 0;
    std::vector<std::string> Errors;
    std::vector<std::string> Warnings;
};

struct SNAPI_ASSETPIPELINE_API PluginInfo
{
    std::string Name;
    std::string Version;
    std::string Path;
};

struct SNAPI_ASSETPIPELINE_API ImporterInfo
{
    std::string Name;
    std::string PluginName;
};

struct SNAPI_ASSETPIPELINE_API CookerInfo
{
    std::string Name;
    std::string PluginName;
};

struct SNAPI_ASSETPIPELINE_API CookedAsset
{
    AssetId Id;
    std::string LogicalName;
    TypeId AssetKind;
    TypedPayload Cooked;
    std::vector<BulkChunk> Bulk;
    bool bDirty = true;
};

struct SNAPI_ASSETPIPELINE_API PipelineResult
{
    AssetId Id;
    std::string LogicalName;
};

class SNAPI_ASSETPIPELINE_API AssetPipelineEngine
{
public:
    AssetPipelineEngine();
    ~AssetPipelineEngine();

    // Initialize with configuration
    std::expected<void, std::string> Initialize(const PipelineBuildConfig& Config);

    // ========== Batch Build ==========

    // Build all assets from scratch
    BuildResult BuildAll();

    // Build only changed assets (incremental)
    BuildResult BuildChanged();

    // Build a single source file to the output pack
    BuildResult BuildAsset(const std::string& SourcePath,
                           const std::string& OutputPack = "",
                           bool bAppend = true);

    // Build multiple specific source files
    BuildResult BuildAssets(const std::vector<std::string>& SourcePaths,
                            const std::string& OutputPack = "",
                            bool bAppend = true);

    // ========== On-Demand Processing (stores in memory) ==========

    std::expected<PipelineResult, std::string> ProcessSource(
        const std::string& AbsolutePath, const std::string& LogicalName);

    // ========== In-Memory Access ==========

    bool HasAsset(const std::string& LogicalName) const;
    std::expected<AssetId, std::string> GetAssetId(const std::string& LogicalName) const;
    std::expected<std::reference_wrapper<const CookedAsset>, std::string> GetCookedAsset(const std::string& LogicalName) const;
    uint32_t GetDirtyCount() const;

    // ========== Persistence ==========

    std::expected<void, std::string> SaveAll();

    // ========== Inline Registration ==========

    void RegisterImporter(std::unique_ptr<IAssetImporter> Importer);
    void RegisterCooker(std::unique_ptr<IAssetCooker> Cooker);
    void RegisterSerializer(std::unique_ptr<IPayloadSerializer> Serializer);

    // ========== Registry Access ==========

    PayloadRegistry& GetRegistry();
    const PayloadRegistry& GetRegistry() const;

    // ========== Introspection ==========

    [[nodiscard]] std::vector<PluginInfo> GetPlugins() const;
    [[nodiscard]] std::vector<ImporterInfo> GetImporters() const;
    [[nodiscard]] std::vector<CookerInfo> GetCookers() const;

    // Non-copyable
    AssetPipelineEngine(const AssetPipelineEngine&) = delete;
    AssetPipelineEngine& operator=(const AssetPipelineEngine&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace SnAPI::AssetPipeline
