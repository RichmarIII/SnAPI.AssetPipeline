#include "AssetPipeline.h"
#include "AssetPackReader.h"
#include "PipelineBuildConfig.h"

#include <iostream>
#include <string>
#include <vector>
#include <cstring>

using namespace SnAPI::AssetPipeline;

void PrintUsage(const char* ProgramName)
{
  std::cout << "Usage: " << ProgramName << " <command> [options]\n\n"
            << "Commands:\n"
            << "  build              Build all assets from scratch\n"
            << "  build-changed      Build only changed assets (incremental)\n"
            << "  inspect <pack>     Inspect a .snpak file\n"
            << "  list-plugins       List loaded plugins\n"
            << "  help               Show this help message\n\n"
            << "Options:\n"
            << "  -s, --source <dir>       Add source directory (can be used multiple times)\n"
            << "  -o, --output <file>      Output .snpak file path\n"
            << "  -p, --plugin <file>      Load plugin DLL/SO (can be used multiple times)\n"
            << "  -c, --compression <mode> Compression mode: none, lz4, lz4hc, zstd, zstdfast (default: zstd)\n"
            << "  --compression-level <level> Compression level: fast, default, high, max (default: default)\n"
            << "  -v, --verbose            Enable verbose output\n"
            << "  --max-compression        Use maximum compression (slower)\n"
            << std::endl;
}

void CommandInspect(const std::string& PackPath)
{
  AssetPackReader Reader;
  auto Result = Reader.Open(PackPath);

  if (!Result.has_value())
  {
    std::cerr << "Error: " << Result.error() << std::endl;
    return;
  }

  std::cout << "Pack: " << PackPath << "\n"
            << "Assets: " << Reader.GetAssetCount() << "\n\n";

  for (uint32_t i = 0; i < Reader.GetAssetCount(); ++i)
  {
    auto Info = Reader.GetAssetInfo(i);
    if (!Info.has_value())
    {
      std::cerr << "  [" << i << "] Error: " << Info.error() << std::endl;
      continue;
    }

    std::cout << "  [" << i << "] " << Info->Name;
    if (!Info->VariantKey.empty())
    {
      std::cout << " (variant: " << Info->VariantKey << ")";
    }
    std::cout << "\n"
              << "       ID: " << Info->Id.ToString() << "\n"
              << "       Kind: " << Info->AssetKind.ToString() << "\n"
              << "       PayloadType: " << Info->CookedPayloadType.ToString() << "\n"
              << "       SchemaVersion: " << Info->SchemaVersion << "\n"
              << "       BulkChunks: " << Info->BulkChunkCount << "\n";
  }
}

void CommandBuild(const PipelineBuildConfig& Config, bool bIncrementalOnly)
{
  AssetPipelineEngine Engine;

  auto InitResult = Engine.Initialize(Config);
  if (!InitResult.has_value())
  {
    std::cerr << "Error initializing pipeline: " << InitResult.error() << std::endl;
    return;
  }

  BuildResult Result;
  if (bIncrementalOnly)
  {
    std::cout << "Building changed assets..." << std::endl;
    Result = Engine.BuildChanged();
  }
  else
  {
    std::cout << "Building all assets..." << std::endl;
    Result = Engine.BuildAll();
  }

  std::cout << "\nBuild " << (Result.bSuccess ? "succeeded" : "failed") << "\n"
            << "  Assets built: " << Result.AssetsBuilt << "\n"
            << "  Assets skipped: " << Result.AssetsSkipped << "\n"
            << "  Assets failed: " << Result.AssetsFailed << "\n";

  if (!Result.Warnings.empty())
  {
    std::cout << "\nWarnings:\n";
    for (const auto& Warning : Result.Warnings)
    {
      std::cout << "  - " << Warning << "\n";
    }
  }

  if (!Result.Errors.empty())
  {
    std::cerr << "\nErrors:\n";
    for (const auto& Error : Result.Errors)
    {
      std::cerr << "  - " << Error << "\n";
    }
  }
}

void CommandListPlugins(const PipelineBuildConfig& Config)
{
  AssetPipelineEngine Engine;

  auto InitResult = Engine.Initialize(Config);
  if (!InitResult.has_value())
  {
    std::cerr << "Error initializing pipeline: " << InitResult.error() << std::endl;
    return;
  }

  auto Plugins = Engine.GetPlugins();
  auto Importers = Engine.GetImporters();
  auto Cookers = Engine.GetCookers();

  std::cout << "Plugins (" << Plugins.size() << "):\n";
  for (const auto& Plugin : Plugins)
  {
    std::cout << "  - " << Plugin.Name << " v" << Plugin.Version << "\n"
              << "    Path: " << Plugin.Path << "\n";
  }

  std::cout << "\nImporters (" << Importers.size() << "):\n";
  for (const auto& Importer : Importers)
  {
    std::cout << "  - " << Importer.Name << " (" << Importer.PluginName << ")\n";
  }

  std::cout << "\nCookers (" << Cookers.size() << "):\n";
  for (const auto& Cooker : Cookers)
  {
    std::cout << "  - " << Cooker.Name << " (" << Cooker.PluginName << ")\n";
  }
}

int main(int argc, char* argv[])
{
  if (argc < 2)
  {
    PrintUsage(argv[0]);
    return 1;
  }

  std::string Command = argv[1];

  if (Command == "help" || Command == "-h" || Command == "--help")
  {
    PrintUsage(argv[0]);
    return 0;
  }

  // Parse options
  PipelineBuildConfig Config;

  for (int i = 2; i < argc; ++i)
  {
    std::string Arg = argv[i];

    if ((Arg == "-s" || Arg == "--source") && i + 1 < argc)
    {
      Config.SourceRoots.push_back(argv[++i]);
    }
    else if ((Arg == "-o" || Arg == "--output") && i + 1 < argc)
    {
      Config.OutputPackPath = argv[++i];
    }
    else if ((Arg == "-p" || Arg == "--plugin") && i + 1 < argc)
    {
      Config.PluginPaths.push_back(argv[++i]);
    }
    else if ((Arg == "-c" || Arg == "--compression") && i + 1 < argc)
    {
      std::string Mode = argv[++i];
      if (Mode == "none")
      {
        Config.Compression = EPackCompression::None;
      }
      else if (Mode == "lz4")
      {
        Config.Compression = EPackCompression::LZ4;
      }
      else if (Mode == "lz4hc")
      {
        Config.Compression = EPackCompression::LZ4HC;
      }
      else if (Mode == "zstd")
      {
        Config.Compression = EPackCompression::Zstd;
      }
      else if (Mode == "zstdfast")
      {
        Config.Compression = EPackCompression::ZstdFast;
      }
      else
      {
        std::cerr << "Unknown compression mode: " << Mode << std::endl;
        return 1;
      }
    }
    else if (Arg == "--compression-level" && i + 1 < argc)
    {
      std::string Level = argv[++i];
      if (Level == "fast")
      {
        Config.CompressionLevel = EPackCompressionLevel::Fast;
      }
      else if (Level == "default")
      {
        Config.CompressionLevel = EPackCompressionLevel::Default;
      }
      else if (Level == "high")
      {
        Config.CompressionLevel = EPackCompressionLevel::High;
      }
      else if (Level == "max")
      {
        Config.CompressionLevel = EPackCompressionLevel::Max;
      }
      else
      {
        std::cerr << "Unknown compression level: " << Level << std::endl;
        return 1;
      }
    }
    else if (Arg == "-v" || Arg == "--verbose")
    {
      Config.bVerbose = true;
    }
    else if (Arg == "--max-compression")
    {
      Config.CompressionLevel = EPackCompressionLevel::Max;
    }
    else if (!Arg.empty() && Arg[0] != '-')
    {
      // Positional argument (e.g., pack path for inspect)
      if (Command == "inspect" && Config.OutputPackPath.empty())
      {
        Config.OutputPackPath = Arg;
      }
    }
    else
    {
      std::cerr << "Unknown option: " << Arg << std::endl;
      return 1;
    }
  }

  // Execute command
  if (Command == "build")
  {
    if (Config.OutputPackPath.empty())
    {
      std::cerr << "Error: Output path (-o) is required for build\n";
      return 1;
    }
    if (Config.SourceRoots.empty())
    {
      std::cerr << "Error: At least one source directory (-s) is required for build\n";
      return 1;
    }
    CommandBuild(Config, false);
  }
  else if (Command == "build-changed")
  {
    if (Config.OutputPackPath.empty())
    {
      std::cerr << "Error: Output path (-o) is required for build-changed\n";
      return 1;
    }
    if (Config.SourceRoots.empty())
    {
      std::cerr << "Error: At least one source directory (-s) is required for build-changed\n";
      return 1;
    }
    CommandBuild(Config, true);
  }
  else if (Command == "inspect")
  {
    if (Config.OutputPackPath.empty())
    {
      std::cerr << "Error: Pack file path is required for inspect\n";
      return 1;
    }
    CommandInspect(Config.OutputPackPath);
  }
  else if (Command == "list-plugins")
  {
    CommandListPlugins(Config);
  }
  else
  {
    std::cerr << "Unknown command: " << Command << std::endl;
    PrintUsage(argv[0]);
    return 1;
  }

  return 0;
}
