#pragma once

#include "TextureCompressorPayloads.h"

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace TextureCompressorPlugin
{

class CompressorBackendBCn
{
public:
  // Compress a single mip level to the specified BCn format.
  // Input: RGBA8 pixel data (Width * Height * 4 bytes).
  // Returns: compressed block data.
  static std::expected<std::vector<uint8_t>, std::string> Compress(
      const uint8_t* Pixels,
      uint32_t Width, uint32_t Height,
      ECompressedFormat Format,
      float Quality);

  // Calculate the compressed size for a given dimension and format
  static uint32_t CalculateCompressedSize(uint32_t Width, uint32_t Height, ECompressedFormat Format);

  // Check if the backend is available (library linked)
  static bool IsAvailable();
};

} // namespace TextureCompressorPlugin
