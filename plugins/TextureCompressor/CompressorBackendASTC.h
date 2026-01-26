#pragma once

#include "TextureCompressorPayloads.h"

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace TextureCompressorPlugin
{

class CompressorBackendASTC
{
public:
  // Compress a single mip level to ASTC.
  // Input: RGBA8 pixel data (Width * Height * 4 bytes) for LDR,
  //        or float RGBA (Width * Height * 16 bytes) for HDR.
  // Returns: compressed ASTC block data (16 bytes per block).
  static std::expected<std::vector<uint8_t>, std::string> Compress(
      const uint8_t* Pixels,
      uint32_t Width, uint32_t Height,
      uint32_t BlockW, uint32_t BlockH,
      float Quality,
      bool bHDR);

  // Calculate the compressed size for given dimensions and block size
  static uint32_t CalculateCompressedSize(uint32_t Width, uint32_t Height,
                                          uint32_t BlockW, uint32_t BlockH);

  // Check if the backend is available (library linked)
  static bool IsAvailable();
};

} // namespace TextureCompressorPlugin
