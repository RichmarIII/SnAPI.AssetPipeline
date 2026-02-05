#pragma once

namespace SnAPI::AssetPipeline
{

// Compression mode for writing packs
enum class EPackCompression
{
    None,
    LZ4,
    LZ4HC,
    Zstd,
    ZstdFast,
};

enum class EPackCompressionLevel
{
    Default,
    Fast,
    High,
    Max,
};

} // namespace SnAPI::AssetPipeline
