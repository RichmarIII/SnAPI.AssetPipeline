#pragma once

#include "SourceMountConfig.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace SnAPI::AssetPipeline
{

struct ResolvedSource
{
    std::string AbsolutePath;
    std::string LogicalName; // Relative path from root = what the user queries by
};

class SourceAssetResolver
{
  public:
    void AddRoot(const SourceMountConfig& Config);
    void RemoveRoot(const std::string& RootPath);
    std::optional<ResolvedSource> Resolve(const std::string& Name) const;

  private:
    std::vector<SourceMountConfig> m_Roots;
};

} // namespace SnAPI::AssetPipeline
