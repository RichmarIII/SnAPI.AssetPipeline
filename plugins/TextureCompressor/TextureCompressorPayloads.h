#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace TextureCompressorPlugin
{

// Compressed texture format enum (covers both BCn and ASTC)
enum class ECompressedFormat : uint32_t
{
  Unknown = 0,

  // BCn (Desktop - DirectX/Vulkan)
  BC1 = 1,   // RGB, 1-bit alpha, 8:1
  BC3 = 2,   // RGBA, explicit alpha, 4:1
  BC4 = 3,   // Single channel, 2:1
  BC5 = 4,   // Two channels (normal XY), 2:1
  BC6H = 5,  // HDR RGB (half-float), 6:1
  BC7 = 6,   // High-quality RGBA, 4:1

  // ASTC (Mobile - Vulkan/Metal/OpenGL ES)
  ASTC_4x4 = 10,   // 8 bpp (highest quality)
  ASTC_5x5 = 11,   // 5.12 bpp
  ASTC_6x6 = 12,   // 3.56 bpp
  ASTC_8x8 = 13,   // 2 bpp
  ASTC_10x10 = 14, // 1.28 bpp
  ASTC_12x12 = 15, // 0.89 bpp

  // ASTC HDR variants
  ASTC_4x4_HDR = 20,
  ASTC_6x6_HDR = 21,
  ASTC_8x8_HDR = 22,
};

// Rich intermediate payload from importer
struct ImageIntermediate
{
  uint32_t Width = 0;
  uint32_t Height = 0;
  uint32_t Channels = 4;
  uint32_t BitsPerChannel = 8;    // 8, 16, or 32
  bool bIsFloat = false;           // true for HDR (EXR, HDR)
  bool bHasNonTrivialAlpha = false;
  bool bSRGB = true;
  std::string SourceFilename;
  std::vector<uint8_t> Pixels;    // RGBA8 for LDR, float RGBA for HDR

  size_t GetPixelCount() const { return static_cast<size_t>(Width) * Height; }
  size_t GetBytesPerPixel() const { return static_cast<size_t>(Channels) * (BitsPerChannel / 8); }
  size_t GetByteCount() const { return GetPixelCount() * GetBytesPerPixel(); }
};

// Mip level metadata in cooked info
struct MipLevelInfo
{
  uint32_t Width = 0;
  uint32_t Height = 0;
  uint32_t CompressedSize = 0;
};

// Cooked payload: compressed texture metadata
struct TextureCompressorCookedInfo
{
  uint32_t BaseWidth = 0;
  uint32_t BaseHeight = 0;
  uint32_t MipCount = 0;
  ECompressedFormat Format = ECompressedFormat::Unknown;
  bool bSRGB = true;
  uint32_t SourceChannels = 4;
  uint32_t BlockWidth = 4;   // 4 for BCn, variable for ASTC
  uint32_t BlockHeight = 4;
  std::vector<MipLevelInfo> MipLevels;
};

// Runtime load parameters (passed via std::any through AssetLoadContext::Params)
struct TextureCompressorLoadParams
{
  int32_t MaxMipLevel = -1;   // -1 = load all mips
  bool bStreamMips = false;
};

// Helper: get block dimensions for a given format
inline void GetBlockDimensions(ECompressedFormat Format, uint32_t& OutW, uint32_t& OutH)
{
  switch (Format)
  {
  case ECompressedFormat::BC1:
  case ECompressedFormat::BC3:
  case ECompressedFormat::BC4:
  case ECompressedFormat::BC5:
  case ECompressedFormat::BC6H:
  case ECompressedFormat::BC7:
    OutW = 4;
    OutH = 4;
    break;
  case ECompressedFormat::ASTC_4x4:
  case ECompressedFormat::ASTC_4x4_HDR:
    OutW = 4;
    OutH = 4;
    break;
  case ECompressedFormat::ASTC_5x5:
    OutW = 5;
    OutH = 5;
    break;
  case ECompressedFormat::ASTC_6x6:
  case ECompressedFormat::ASTC_6x6_HDR:
    OutW = 6;
    OutH = 6;
    break;
  case ECompressedFormat::ASTC_8x8:
  case ECompressedFormat::ASTC_8x8_HDR:
    OutW = 8;
    OutH = 8;
    break;
  case ECompressedFormat::ASTC_10x10:
    OutW = 10;
    OutH = 10;
    break;
  case ECompressedFormat::ASTC_12x12:
    OutW = 12;
    OutH = 12;
    break;
  default:
    OutW = 4;
    OutH = 4;
    break;
  }
}

// Helper: get bytes per block for a given format
inline uint32_t GetBytesPerBlock(ECompressedFormat Format)
{
  switch (Format)
  {
  case ECompressedFormat::BC1:
  case ECompressedFormat::BC4:
    return 8;
  case ECompressedFormat::BC3:
  case ECompressedFormat::BC5:
  case ECompressedFormat::BC6H:
  case ECompressedFormat::BC7:
    return 16;
  // All ASTC formats: 16 bytes per block regardless of block size
  case ECompressedFormat::ASTC_4x4:
  case ECompressedFormat::ASTC_5x5:
  case ECompressedFormat::ASTC_6x6:
  case ECompressedFormat::ASTC_8x8:
  case ECompressedFormat::ASTC_10x10:
  case ECompressedFormat::ASTC_12x12:
  case ECompressedFormat::ASTC_4x4_HDR:
  case ECompressedFormat::ASTC_6x6_HDR:
  case ECompressedFormat::ASTC_8x8_HDR:
    return 16;
  default:
    return 16;
  }
}

// Helper: is the format an ASTC format?
inline bool IsASTCFormat(ECompressedFormat Format)
{
  return static_cast<uint32_t>(Format) >= 10;
}

// Helper: is the format an HDR format?
inline bool IsHDRFormat(ECompressedFormat Format)
{
  return Format == ECompressedFormat::BC6H ||
         Format == ECompressedFormat::ASTC_4x4_HDR ||
         Format == ECompressedFormat::ASTC_6x6_HDR ||
         Format == ECompressedFormat::ASTC_8x8_HDR;
}

// Helper: format name string
inline const char* GetFormatName(ECompressedFormat Format)
{
  switch (Format)
  {
  case ECompressedFormat::BC1: return "BC1";
  case ECompressedFormat::BC3: return "BC3";
  case ECompressedFormat::BC4: return "BC4";
  case ECompressedFormat::BC5: return "BC5";
  case ECompressedFormat::BC6H: return "BC6H";
  case ECompressedFormat::BC7: return "BC7";
  case ECompressedFormat::ASTC_4x4: return "ASTC_4x4";
  case ECompressedFormat::ASTC_5x5: return "ASTC_5x5";
  case ECompressedFormat::ASTC_6x6: return "ASTC_6x6";
  case ECompressedFormat::ASTC_8x8: return "ASTC_8x8";
  case ECompressedFormat::ASTC_10x10: return "ASTC_10x10";
  case ECompressedFormat::ASTC_12x12: return "ASTC_12x12";
  case ECompressedFormat::ASTC_4x4_HDR: return "ASTC_4x4_HDR";
  case ECompressedFormat::ASTC_6x6_HDR: return "ASTC_6x6_HDR";
  case ECompressedFormat::ASTC_8x8_HDR: return "ASTC_8x8_HDR";
  default: return "Unknown";
  }
}

} // namespace TextureCompressorPlugin
