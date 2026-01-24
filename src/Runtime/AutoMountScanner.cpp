#include "Runtime/AutoMountScanner.h"

#include <filesystem>

namespace SnAPI::AssetPipeline
{

  std::vector<std::string> AutoMountScanner::Scan(const std::vector<std::string>& Directories)
  {
    std::vector<std::string> Results;

    for (const auto& Dir : Directories)
    {
      try
      {
        if (!std::filesystem::exists(Dir) || !std::filesystem::is_directory(Dir))
        {
          continue;
        }

        for (const auto& Entry : std::filesystem::recursive_directory_iterator(Dir))
        {
          if (Entry.is_regular_file() && Entry.path().extension() == ".snpak")
          {
            Results.push_back(Entry.path().string());
          }
        }
      }
      catch (...)
      {
        // Skip directories that can't be accessed
      }
    }

    return Results;
  }

} // namespace SnAPI::AssetPipeline
