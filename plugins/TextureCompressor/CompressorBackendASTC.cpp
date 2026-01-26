#include "CompressorBackendASTC.h"

#include <algorithm>
#include <cstring>

#if defined(HAS_ASTCENC) && HAS_ASTCENC
#include "astcenc.h"
#endif

namespace TextureCompressorPlugin
{

bool CompressorBackendASTC::IsAvailable()
{
#if defined(HAS_ASTCENC) && HAS_ASTCENC
  return true;
#else
  return false;
#endif
}

uint32_t CompressorBackendASTC::CalculateCompressedSize(
    uint32_t Width, uint32_t Height,
    uint32_t BlockW, uint32_t BlockH)
{
  // Number of blocks needed (rounding up)
  uint32_t BlocksX = (Width + BlockW - 1) / BlockW;
  uint32_t BlocksY = (Height + BlockH - 1) / BlockH;
  // ASTC: always 16 bytes per block
  return BlocksX * BlocksY * 16;
}

std::expected<std::vector<uint8_t>, std::string> CompressorBackendASTC::Compress(
    const uint8_t* Pixels,
    uint32_t Width, uint32_t Height,
    uint32_t BlockW, uint32_t BlockH,
    float Quality,
    bool bHDR)
{
  if (Width == 0 || Height == 0)
  {
    return std::unexpected("Invalid dimensions for ASTC compression");
  }

#if defined(HAS_ASTCENC) && HAS_ASTCENC
  // Map quality to astc-encoder preset
  float AstcQuality;
  if (Quality <= 0.1f)
    AstcQuality = ASTCENC_PRE_FASTEST;
  else if (Quality <= 0.3f)
    AstcQuality = ASTCENC_PRE_FAST;
  else if (Quality <= 0.6f)
    AstcQuality = ASTCENC_PRE_MEDIUM;
  else if (Quality <= 0.85f)
    AstcQuality = ASTCENC_PRE_THOROUGH;
  else
    AstcQuality = ASTCENC_PRE_EXHAUSTIVE;

  // Configure the encoder
  astcenc_config Config;
  astcenc_profile Profile = bHDR ? ASTCENC_PRF_HDR : ASTCENC_PRF_LDR_SRGB;
  unsigned int Flags = 0;

  astcenc_error Status = astcenc_config_init(
      Profile, BlockW, BlockH, 1, // 2D textures (depth = 1)
      AstcQuality, Flags, &Config);

  if (Status != ASTCENC_SUCCESS)
  {
    return std::unexpected("astc-encoder config init failed: " +
                           std::string(astcenc_get_error_string(Status)));
  }

  // Create context
  astcenc_context* Context = nullptr;
  unsigned int ThreadCount = 1;
  Status = astcenc_context_alloc(&Config, ThreadCount, &Context);
  if (Status != ASTCENC_SUCCESS)
  {
    return std::unexpected("astc-encoder context alloc failed: " +
                           std::string(astcenc_get_error_string(Status)));
  }

  // Set up the image
  astcenc_image Image;
  Image.dim_x = Width;
  Image.dim_y = Height;
  Image.dim_z = 1;
  Image.data_type = bHDR ? ASTCENC_TYPE_F32 : ASTCENC_TYPE_U8;

  // astcenc expects array of row pointers
  std::vector<void*> Slices(1);
  Slices[0] = const_cast<uint8_t*>(Pixels);
  Image.data = Slices.data();

  // Calculate output size
  uint32_t CompressedSize = CalculateCompressedSize(Width, Height, BlockW, BlockH);
  std::vector<uint8_t> CompressedData(CompressedSize);

  // Compress
  astcenc_swizzle Swizzle = {ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
  Status = astcenc_compress_image(Context, &Image, &Swizzle,
                                  CompressedData.data(), CompressedData.size(),
                                  0); // thread_index
  if (Status != ASTCENC_SUCCESS)
  {
    astcenc_context_free(Context);
    return std::unexpected("astc-encoder compression failed: " +
                           std::string(astcenc_get_error_string(Status)));
  }

  astcenc_context_free(Context);
  return CompressedData;

#else
  // Fallback: produce correctly-sized dummy compressed data for testing
  uint32_t CompressedSize = CalculateCompressedSize(Width, Height, BlockW, BlockH);
  std::vector<uint8_t> DummyData(CompressedSize, 0);

  // Fill with a pattern for test verification
  uint32_t BlocksX = (Width + BlockW - 1) / BlockW;
  uint32_t BlocksY = (Height + BlockH - 1) / BlockH;

  for (uint32_t By = 0; By < BlocksY; ++By)
  {
    for (uint32_t Bx = 0; Bx < BlocksX; ++Bx)
    {
      size_t BlockIdx = static_cast<size_t>(By) * BlocksX + Bx;
      size_t Offset = BlockIdx * 16; // 16 bytes per ASTC block

      // Sample the top-left pixel of this block
      uint32_t Px = std::min(Bx * BlockW, Width - 1);
      uint32_t Py = std::min(By * BlockH, Height - 1);
      uint32_t BytesPerPixel = bHDR ? 16 : 4;
      size_t PixIdx = (static_cast<size_t>(Py) * Width + Px) * BytesPerPixel;

      // Store first 4 bytes of pixel data as pattern
      for (uint32_t I = 0; I < std::min(static_cast<uint32_t>(16), std::min(BytesPerPixel, 4u)); ++I)
      {
        DummyData[Offset + I] = Pixels[PixIdx + I];
      }
    }
  }

  return DummyData;
#endif
}

} // namespace TextureCompressorPlugin
