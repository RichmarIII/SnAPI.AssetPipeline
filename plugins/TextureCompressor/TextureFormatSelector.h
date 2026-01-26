#pragma once

#include "TextureCompressorPayloads.h"

#include <string>
#include <unordered_map>

namespace TextureCompressorPlugin
{

struct FormatSelection
{
  ECompressedFormat Format = ECompressedFormat::BC7;
  bool bSRGB = true;
  float Quality = 0.5f; // 0.0 = fastest, 1.0 = highest quality
};

class TextureFormatSelector
{
public:
  // Select the best format based on image properties and build options
  static FormatSelection Select(
      const ImageIntermediate& Image,
      const std::unordered_map<std::string, std::string>& BuildOptions);

private:
  // Parse format override from BuildOptions
  static bool TryParseFormatOverride(
      const std::unordered_map<std::string, std::string>& Options,
      ECompressedFormat& OutFormat);

  // Determine target family from options ("bcn" or "astc")
  static std::string GetTarget(const std::unordered_map<std::string, std::string>& Options);

  // Check if filename indicates a normal map
  static bool IsNormalMapFilename(const std::string& Filename);

  // Check if filename indicates HDR
  static bool IsHDRFilename(const std::string& Filename);

  // BCn heuristics
  static FormatSelection SelectBCn(const ImageIntermediate& Image,
                                   const std::unordered_map<std::string, std::string>& Options);

  // ASTC heuristics
  static FormatSelection SelectASTC(const ImageIntermediate& Image,
                                    const std::unordered_map<std::string, std::string>& Options);
};

} // namespace TextureCompressorPlugin
