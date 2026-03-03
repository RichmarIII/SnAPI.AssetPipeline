#include "CompressorBackendBCn.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

#if defined(HAS_COMPRESSONATOR) && HAS_COMPRESSONATOR
#include "compressonator.h"
#endif

namespace TextureCompressorPlugin
{
namespace
{
constexpr uint32_t kThreadCountFallback = 8u;
constexpr uint32_t kThreadCountMax = 128u; // Compressonator BC7/BC6H documented max.
// Compressonator public headers do not expose named BC7 mode-bit constants.
// Try all BC7 modes (0-7) for highest quality.
constexpr uint32_t kBC7ModeMaskDefault = 0xFFu;
} // namespace

bool CompressorBackendBCn::IsAvailable()
{
#if defined(HAS_COMPRESSONATOR) && HAS_COMPRESSONATOR
  return true;
#else
  return false;
#endif
}

uint32_t CompressorBackendBCn::CalculateCompressedSize(uint32_t Width, uint32_t Height, ECompressedFormat Format)
{
  // Align to 4x4 blocks
  uint32_t BlockW = (Width + 3) / 4;
  uint32_t BlockH = (Height + 3) / 4;
  uint32_t BytesPerBlock = GetBytesPerBlock(Format);
  return BlockW * BlockH * BytesPerBlock;
}

std::expected<std::vector<uint8_t>, std::string> CompressorBackendBCn::Compress(
    const uint8_t* Pixels,
    uint32_t Width, uint32_t Height,
    ECompressedFormat Format,
    float Quality)
{
  if (Width == 0 || Height == 0)
  {
    return std::unexpected("Invalid dimensions for BCn compression");
  }

#if defined(HAS_COMPRESSONATOR) && HAS_COMPRESSONATOR
  // Map our format enum to Compressonator format
  CMP_FORMAT CmpFormat;
  switch (Format)
  {
  case ECompressedFormat::BC1: CmpFormat = CMP_FORMAT_BC1; break;
  case ECompressedFormat::BC3: CmpFormat = CMP_FORMAT_BC3; break;
  case ECompressedFormat::BC4: CmpFormat = CMP_FORMAT_BC4; break;
  case ECompressedFormat::BC5: CmpFormat = CMP_FORMAT_BC5; break;
  case ECompressedFormat::BC6H: CmpFormat = CMP_FORMAT_BC6H; break;
  case ECompressedFormat::BC7: CmpFormat = CMP_FORMAT_BC7; break;
  default:
    return std::unexpected("Unsupported BCn format: " + std::string(GetFormatName(Format)));
  }

  // Source pixels are RGBA8 from ImageIntermediate.
  //
  // Important: in our vendored Compressonator build (common.h defines
  // USE_OLD_SWIZZLE), the fastest + color-correct BC7 path is to keep the
  // byte stream as-is and tag the source as BGRA_8888. Do not add a manual
  // pre-swizzle here unless you also re-validate color and throughput.
  // (ARGB/BGRA pre-swizzle experiments were substantially slower and/or
  // produced channel inversion in this configuration.)
  CMP_Texture SrcTexture = {};
  SrcTexture.dwSize = sizeof(CMP_Texture);
  SrcTexture.dwWidth = Width;
  SrcTexture.dwHeight = Height;
  SrcTexture.dwPitch = Width * 4;
  SrcTexture.format = CMP_FORMAT_BGRA_8888;
  SrcTexture.dwDataSize = Width * Height * 4;
  SrcTexture.pData = const_cast<uint8_t*>(Pixels);

  // Create destination texture
  CMP_Texture DstTexture = {};
  DstTexture.dwSize = sizeof(CMP_Texture);
  DstTexture.dwWidth = Width;
  DstTexture.dwHeight = Height;
  DstTexture.dwPitch = 0;
  DstTexture.format = CmpFormat;
  DstTexture.dwDataSize = CMP_CalculateBufferSize(&DstTexture);

  std::vector<uint8_t> CompressedData(DstTexture.dwDataSize);
  DstTexture.pData = CompressedData.data();

  // Set compression options
  CMP_CompressOptions Options = {};
  Options.dwSize = sizeof(CMP_CompressOptions);
  Options.fquality = Quality;
  Options.bDisableMultiThreading = false;

  // Always request full thread usage from Compressonator.
  const uint32_t HwThreads = std::thread::hardware_concurrency();
  const uint32_t RequestedThreads = (HwThreads > 0u) ? HwThreads : kThreadCountFallback;
  Options.dwnumThreads = std::min(RequestedThreads, kThreadCountMax);

  if (Format == ECompressedFormat::BC7)
  {
    // Avoid zero-initialized invalid mode-mask and use named, explicit modes.
    Options.dwmodeMask = kBC7ModeMaskDefault;
    Options.brestrictColour = false;
    Options.brestrictAlpha = false;
  }

  // Perform compression
  CMP_ERROR Status = CMP_ConvertTexture(&SrcTexture, &DstTexture, &Options, nullptr);
  if (Status != CMP_OK)
  {
    return std::unexpected("Compressonator compression failed with error: " + std::to_string(static_cast<int>(Status)));
  }

  CompressedData.resize(DstTexture.dwDataSize);
  return CompressedData;

#else
  // Fallback: produce correctly-sized dummy compressed data for testing
  uint32_t CompressedSize = CalculateCompressedSize(Width, Height, Format);
  std::vector<uint8_t> DummyData(CompressedSize, 0);

  // Fill with a pattern derived from source pixels for testing distinguishability
  uint32_t BlockW = (Width + 3) / 4;
  uint32_t BlockH = (Height + 3) / 4;
  uint32_t BytesPerBlock = GetBytesPerBlock(Format);

  for (uint32_t By = 0; By < BlockH; ++By)
  {
    for (uint32_t Bx = 0; Bx < BlockW; ++Bx)
    {
      size_t BlockIdx = static_cast<size_t>(By) * BlockW + Bx;
      size_t Offset = BlockIdx * BytesPerBlock;

      // Sample the top-left pixel of this 4x4 block
      uint32_t Px = std::min(Bx * 4, Width - 1);
      uint32_t Py = std::min(By * 4, Height - 1);
      size_t PixIdx = (static_cast<size_t>(Py) * Width + Px) * 4;

      // Store R,G,B,A as first bytes of block (for test verification)
      for (uint32_t I = 0; I < std::min(BytesPerBlock, 4u); ++I)
      {
        DummyData[Offset + I] = Pixels[PixIdx + I];
      }
    }
  }

  return DummyData;
#endif
}

} // namespace TextureCompressorPlugin
