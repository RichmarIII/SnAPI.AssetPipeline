#pragma once

#include <string>
#include <vector>

namespace SnAPI::AssetPipeline
{

class AutoMountScanner
{
  public:
    // Recursively finds all .snpak files in the given directories
    static std::vector<std::string> Scan(const std::vector<std::string>& Directories);
};

} // namespace SnAPI::AssetPipeline
