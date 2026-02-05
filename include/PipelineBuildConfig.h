#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "PackCompression.h"
#include "Export.h"

namespace SnAPI::AssetPipeline
{

struct SNAPI_ASSETPIPELINE_API PipelineBuildConfig
{
    // Source directories to scan for assets
    std::vector<std::string> SourceRoots;

    // Paths to plugin DLLs/SOs
    std::vector<std::string> PluginPaths;

    // Output .snpak file path
    std::string OutputPackPath;

    // Build options passed to importers/cookers
    std::unordered_map<std::string, std::string> BuildOptions;

    // Use deterministic UUIDs for asset IDs
    bool bDeterministicAssetIds = true;

    // Enable append-update mode (fast incremental packaging)
    bool bEnableAppendUpdates = true;

    // Cache database path for incremental builds (empty = derived from OutputPackPath)
    std::string CacheDatabasePath;

    // Compression settings
    EPackCompression Compression = EPackCompression::Zstd;
    EPackCompressionLevel CompressionLevel = EPackCompressionLevel::Default;

    // Number of parallel jobs (0 = auto)
    uint32_t ParallelJobs = 0;

    // Verbose logging
    bool bVerbose = false;
};

} // namespace AssetPipeline
