#pragma once

#include <cstdint>
#include <vector>

namespace TextureCompressorPlugin
{

struct MipLevel
{
  uint32_t Width = 0;
  uint32_t Height = 0;
  std::vector<uint8_t> Pixels; // RGBA8 or float RGBA depending on source
};

class MipGenerator
{
public:
  // Generate a full mip chain from the given source pixels.
  // Returns the chain starting from the source (mip 0) down to 1x1.
  // If MaxMipCount > 0, limits the number of mips generated (including mip 0).
  // bLinear: if true, operates in linear space (for normal maps);
  //          if false, converts sRGB->linear before averaging, then back.
  static std::vector<MipLevel> Generate(
      const uint8_t* SrcPixels,
      uint32_t Width, uint32_t Height,
      uint32_t Channels,
      bool bSRGB,
      int32_t MaxMipCount = -1);

  // Calculate how many mip levels a given dimension would produce
  static uint32_t CalculateMipCount(uint32_t Width, uint32_t Height);

private:
  static float SRGBToLinear(float Value);
  static float LinearToSRGB(float Value);
  static void DownsampleBoxFilter(
      const uint8_t* Src, uint32_t SrcW, uint32_t SrcH,
      uint8_t* Dst, uint32_t DstW, uint32_t DstH,
      uint32_t Channels, bool bSRGB);
};

} // namespace TextureCompressorPlugin
