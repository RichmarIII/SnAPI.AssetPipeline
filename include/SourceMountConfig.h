#pragma once

#include <cstdint>
#include <string>

#include "Export.h"

namespace SnAPI::AssetPipeline
{

struct SNAPI_ASSETPIPELINE_API SourceMountConfig
{
    std::string RootPath;        // Absolute path to source directory
    int32_t Priority = 0;        // Higher priority roots are searched first
    std::string MountPoint = ""; // Virtual prefix (e.g., "dlc1/")
};

} // namespace SnAPI::AssetPipeline
