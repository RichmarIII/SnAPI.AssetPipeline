#pragma once

#include <memory>
#include <expected>
#include <string>
#include <vector>

#include "Export.h"
#include "Uuid.h"
#include "PipelineBuildConfig.h"

namespace SnAPI::AssetPipeline
{

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

class SNAPI_ASSETPIPELINE_API AssetPipelineEngine
{
public:
    AssetPipelineEngine();
    ~AssetPipelineEngine();

    // Initialize with configuration
    std::expected<void, std::string> Initialize(const PipelineBuildConfig& Config) const;

    // Build all assets from scratch
    BuildResult BuildAll() const;

    // Build only changed assets (incremental)
    BuildResult BuildChanged() const;

    // Build a single source file to the output pack
    // sourcePath: path to the source asset file
    // outputPack: optional override for output pack (uses Config.OutputPackPath if empty)
    // bAppend: if true, appends to existing pack; if false, replaces matching assets
    BuildResult BuildAsset(const std::string& SourcePath,
                           const std::string& OutputPack = "",
                           bool bAppend = true) const;

    // Build multiple specific source files
    BuildResult BuildAssets(const std::vector<std::string>& SourcePaths,
                            const std::string& OutputPack = "",
                            bool bAppend = true) const;

    // Get loaded plugin info
    [[nodiscard]] std::vector<PluginInfo> GetPlugins() const;

    // Get registered importers
    [[nodiscard]] std::vector<ImporterInfo> GetImporters() const;

    // Get registered cookers
    [[nodiscard]] std::vector<CookerInfo> GetCookers() const;

    // Non-copyable
    AssetPipelineEngine(const AssetPipelineEngine&) = delete;
    AssetPipelineEngine& operator=(const AssetPipelineEngine&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace AssetPipeline
