#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "Export.h"
#include "PipelineBuildConfig.h"

namespace SnAPI::AssetPipeline
{

// Configuration for source asset mount points
struct SNAPI_ASSETPIPELINE_API SourceMountConfig
{
    std::string RootPath;        // Absolute path to source directory
    int32_t Priority = 0;        // Higher priority roots are searched first
    std::string MountPoint = ""; // Virtual prefix (e.g., "dlc1/")
};

// Configuration for the runtime pipeline (on-the-fly asset cooking)
struct SNAPI_ASSETPIPELINE_API RuntimePipelineConfig
{
    // Paths to plugin DLLs/SOs to load
    std::vector<std::string> PluginPaths;

    // Directory where the runtime snpak is stored
    std::string OutputDirectory;

    // Name of the runtime pack file
    std::string RuntimePackName = "runtime_assets.snpak";

    // Compression mode for runtime pack
    PipelineBuildConfig::ECompressionMode CompressionMode = PipelineBuildConfig::ECompressionMode::LZ4;

    // Build options passed to importers/cookers
    std::unordered_map<std::string, std::string> BuildOptions;

    // Save runtime pack on AssetManager destruction
    bool bAutoSave = false;

    // Use deterministic UUIDs for asset IDs
    bool bDeterministicAssetIds = true;
};

} // namespace SnAPI::AssetPipeline
