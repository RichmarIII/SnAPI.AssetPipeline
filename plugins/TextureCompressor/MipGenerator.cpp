#include "MipGenerator.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace TextureCompressorPlugin
{

uint32_t MipGenerator::CalculateMipCount(uint32_t Width, uint32_t Height)
{
  uint32_t Count = 1;
  uint32_t W = Width;
  uint32_t H = Height;
  while (W > 1 || H > 1)
  {
    W = std::max(1u, W / 2);
    H = std::max(1u, H / 2);
    ++Count;
  }
  return Count;
}

float MipGenerator::SRGBToLinear(float Value)
{
  if (Value <= 0.04045f)
    return Value / 12.92f;
  return std::pow((Value + 0.055f) / 1.055f, 2.4f);
}

float MipGenerator::LinearToSRGB(float Value)
{
  Value = std::clamp(Value, 0.0f, 1.0f);
  if (Value <= 0.0031308f)
    return Value * 12.92f;
  return 1.055f * std::pow(Value, 1.0f / 2.4f) - 0.055f;
}

void MipGenerator::DownsampleBoxFilter(
    const uint8_t* Src, uint32_t SrcW, uint32_t SrcH,
    uint8_t* Dst, uint32_t DstW, uint32_t DstH,
    uint32_t Channels, bool bSRGB)
{
  for (uint32_t DstY = 0; DstY < DstH; ++DstY)
  {
    for (uint32_t DstX = 0; DstX < DstW; ++DstX)
    {
      // Source region (2x2 box, clamped)
      uint32_t SrcX0 = DstX * 2;
      uint32_t SrcY0 = DstY * 2;
      uint32_t SrcX1 = std::min(SrcX0 + 1, SrcW - 1);
      uint32_t SrcY1 = std::min(SrcY0 + 1, SrcH - 1);

      for (uint32_t C = 0; C < Channels; ++C)
      {
        // Gather 4 samples
        float S00 = Src[(static_cast<size_t>(SrcY0) * SrcW + SrcX0) * Channels + C] / 255.0f;
        float S10 = Src[(static_cast<size_t>(SrcY0) * SrcW + SrcX1) * Channels + C] / 255.0f;
        float S01 = Src[(static_cast<size_t>(SrcY1) * SrcW + SrcX0) * Channels + C] / 255.0f;
        float S11 = Src[(static_cast<size_t>(SrcY1) * SrcW + SrcX1) * Channels + C] / 255.0f;

        // sRGB-correct: linearize before averaging (skip alpha channel)
        bool bApplySRGB = bSRGB && (C < 3);
        if (bApplySRGB)
        {
          S00 = SRGBToLinear(S00);
          S10 = SRGBToLinear(S10);
          S01 = SRGBToLinear(S01);
          S11 = SRGBToLinear(S11);
        }

        float Avg = (S00 + S10 + S01 + S11) * 0.25f;

        if (bApplySRGB)
        {
          Avg = LinearToSRGB(Avg);
        }

        Dst[(static_cast<size_t>(DstY) * DstW + DstX) * Channels + C] =
            static_cast<uint8_t>(std::clamp(Avg * 255.0f + 0.5f, 0.0f, 255.0f));
      }
    }
  }
}

std::vector<MipLevel> MipGenerator::Generate(
    const uint8_t* SrcPixels,
    uint32_t Width, uint32_t Height,
    uint32_t Channels,
    bool bSRGB,
    int32_t MaxMipCount)
{
  std::vector<MipLevel> Chain;

  uint32_t TotalMips = CalculateMipCount(Width, Height);
  if (MaxMipCount > 0)
  {
    TotalMips = std::min(TotalMips, static_cast<uint32_t>(MaxMipCount));
  }

  // Mip 0 = source
  MipLevel Mip0;
  Mip0.Width = Width;
  Mip0.Height = Height;
  size_t Mip0Size = static_cast<size_t>(Width) * Height * Channels;
  Mip0.Pixels.assign(SrcPixels, SrcPixels + Mip0Size);
  Chain.push_back(std::move(Mip0));

  // Generate remaining mips
  uint32_t CurW = Width;
  uint32_t CurH = Height;

  for (uint32_t I = 1; I < TotalMips; ++I)
  {
    uint32_t NextW = std::max(1u, CurW / 2);
    uint32_t NextH = std::max(1u, CurH / 2);

    MipLevel Mip;
    Mip.Width = NextW;
    Mip.Height = NextH;
    Mip.Pixels.resize(static_cast<size_t>(NextW) * NextH * Channels);

    DownsampleBoxFilter(
        Chain.back().Pixels.data(), CurW, CurH,
        Mip.Pixels.data(), NextW, NextH,
        Channels, bSRGB);

    CurW = NextW;
    CurH = NextH;
    Chain.push_back(std::move(Mip));
  }

  return Chain;
}

} // namespace TextureCompressorPlugin
