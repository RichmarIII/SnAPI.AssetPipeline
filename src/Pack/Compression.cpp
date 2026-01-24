#include "SnPakFormat.h"

#include <lz4.h>
#include <lz4hc.h>
#include <zstd.h>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace SnAPI::AssetPipeline::Pack
{

  std::vector<uint8_t> Compress(const uint8_t* Data, size_t Size, ESnPakCompression Mode)
  {
    if (Mode == ESnPakCompression::None || Size == 0)
    {
      return std::vector<uint8_t>(Data, Data + Size);
    }

    std::vector<uint8_t> Result;

    if (Mode == ESnPakCompression::LZ4)
    {
      int MaxCompressedSize = LZ4_compressBound(static_cast<int>(Size));
      Result.resize(MaxCompressedSize);

      int CompressedSize = LZ4_compress_HC(reinterpret_cast<const char*>(Data), reinterpret_cast<char*>(Result.data()), static_cast<int>(Size),
                                           MaxCompressedSize, LZ4HC_CLEVEL_DEFAULT);

      if (CompressedSize <= 0)
      {
        throw std::runtime_error("LZ4 compression failed");
      }

      Result.resize(CompressedSize);
    }
    else if (Mode == ESnPakCompression::Zstd)
    {
      size_t MaxCompressedSize = ZSTD_compressBound(Size);
      Result.resize(MaxCompressedSize);

      size_t CompressedSize = ZSTD_compress(Result.data(), Result.size(), Data, Size, ZSTD_defaultCLevel());

      if (ZSTD_isError(CompressedSize))
      {
        throw std::runtime_error(std::string("Zstd compression failed: ") + ZSTD_getErrorName(CompressedSize));
      }

      Result.resize(CompressedSize);
    }

    return Result;
  }

  std::vector<uint8_t> CompressMax(const uint8_t* Data, size_t Size, ESnPakCompression Mode)
  {
    if (Mode == ESnPakCompression::None || Size == 0)
    {
      return std::vector<uint8_t>(Data, Data + Size);
    }

    std::vector<uint8_t> Result;

    if (Mode == ESnPakCompression::LZ4)
    {
      int MaxCompressedSize = LZ4_compressBound(static_cast<int>(Size));
      Result.resize(MaxCompressedSize);

      int CompressedSize = LZ4_compress_HC(reinterpret_cast<const char*>(Data), reinterpret_cast<char*>(Result.data()), static_cast<int>(Size),
                                           MaxCompressedSize, LZ4HC_CLEVEL_MAX);

      if (CompressedSize <= 0)
      {
        throw std::runtime_error("LZ4 compression failed");
      }

      Result.resize(CompressedSize);
    }
    else if (Mode == ESnPakCompression::Zstd)
    {
      size_t MaxCompressedSize = ZSTD_compressBound(Size);
      Result.resize(MaxCompressedSize);

      size_t CompressedSize = ZSTD_compress(Result.data(), Result.size(), Data, Size, ZSTD_maxCLevel());

      if (ZSTD_isError(CompressedSize))
      {
        throw std::runtime_error(std::string("Zstd compression failed: ") + ZSTD_getErrorName(CompressedSize));
      }

      Result.resize(CompressedSize);
    }

    return Result;
  }

  std::vector<uint8_t> Decompress(const uint8_t* Data, size_t CompressedSize, size_t UncompressedSize, ESnPakCompression Mode)
  {
    if (Mode == ESnPakCompression::None || CompressedSize == 0)
    {
      return std::vector<uint8_t>(Data, Data + CompressedSize);
    }

    std::vector<uint8_t> Result(UncompressedSize);

    if (Mode == ESnPakCompression::LZ4)
    {
      int DecompressedSize = LZ4_decompress_safe(reinterpret_cast<const char*>(Data), reinterpret_cast<char*>(Result.data()),
                                                 static_cast<int>(CompressedSize), static_cast<int>(UncompressedSize));

      if (DecompressedSize < 0 || static_cast<size_t>(DecompressedSize) != UncompressedSize)
      {
        throw std::runtime_error("LZ4 decompression failed or size mismatch");
      }
    }
    else if (Mode == ESnPakCompression::Zstd)
    {
      size_t DecompressedSize = ZSTD_decompress(Result.data(), Result.size(), Data, CompressedSize);

      if (ZSTD_isError(DecompressedSize))
      {
        throw std::runtime_error(std::string("Zstd decompression failed: ") + ZSTD_getErrorName(DecompressedSize));
      }

      if (DecompressedSize != UncompressedSize)
      {
        throw std::runtime_error("Zstd decompression size mismatch");
      }
    }

    return Result;
  }

} // namespace SnAPI::AssetPipeline::Pack