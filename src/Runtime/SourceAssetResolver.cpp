#include "Runtime/SourceAssetResolver.h"

#include <filesystem>

namespace SnAPI::AssetPipeline
{

  void SourceAssetResolver::AddRoot(const SourceMountConfig& Config)
  {
    m_Roots.push_back(Config);
    // Sort by priority descending (higher priority first)
    std::sort(m_Roots.begin(), m_Roots.end(), [](const SourceMountConfig& A, const SourceMountConfig& B) { return A.Priority > B.Priority; });
  }

  void SourceAssetResolver::RemoveRoot(const std::string& RootPath)
  {
    m_Roots.erase(std::remove_if(m_Roots.begin(), m_Roots.end(), [&RootPath](const SourceMountConfig& C) { return C.RootPath == RootPath; }),
                  m_Roots.end());
  }

  std::optional<ResolvedSource> SourceAssetResolver::Resolve(const std::string& Name) const
  {
    for (const auto& Root : m_Roots)
    {
      std::string LookupName = Name;

      // If root has a MountPoint, the name must start with it
      if (!Root.MountPoint.empty())
      {
        if (Name.find(Root.MountPoint) == 0)
        {
          LookupName = Name.substr(Root.MountPoint.length());
        }
        else
        {
          continue;
        }
      }

      // Join root path + lookup name
      std::filesystem::path FullPath = std::filesystem::path(Root.RootPath) / LookupName;

      if (std::filesystem::exists(FullPath) && std::filesystem::is_regular_file(FullPath))
      {
        ResolvedSource Result;
        Result.AbsolutePath = FullPath.string();
        Result.LogicalName = Name;
        return Result;
      }
    }

    return std::nullopt;
  }

} // namespace SnAPI::AssetPipeline
