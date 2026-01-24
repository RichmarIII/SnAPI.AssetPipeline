#pragma once

#include <cstdint>
#include <vector>

namespace TexturePlugin
{

// Intermediate payload produced by importer
struct ImageIntermediate
{
    uint32_t Width = 0;
    uint32_t Height = 0;
    uint32_t Channels = 4;             // RGBA
    std::vector<uint8_t> Pixels;       // Width*Height*Channels, RGBA8

    size_t GetPixelCount() const { return Width * Height; }
    size_t GetByteCount() const { return Width * Height * Channels; }
};

// Texture format enum
enum class ETextureFormat : uint32_t
{
    Unknown = 0,
    RGBA8 = 1,
    RGB8 = 2,
    R8 = 3,
    RG8 = 4,
    // Future: BC1, BC3, BC7, ASTC, etc.
};

// Cooked payload (metadata, actual pixels in bulk chunk)
struct TextureCookedInfo
{
    uint32_t Width = 0;
    uint32_t Height = 0;
    uint32_t MipCount = 1;
    ETextureFormat Format = ETextureFormat::RGBA8;

    // Convention: Bulk chunk semantic Reserved_Level, SubIndex=mip
    // Bulk[0] = Mip0 pixel data
};

} // namespace TexturePlugin
