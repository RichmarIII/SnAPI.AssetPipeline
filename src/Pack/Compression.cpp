#include "SnPakFormat.h"

#include <lz4.h>
#include <lz4hc.h>
#include <zstd.h>
#include <stdexcept>
#include <vector>

namespace SnAPI::AssetPipeline::Pack
{

  namespace
  {
    int ClampInt(const int Value, const int MinValue, const int MaxValue)
    {
      if (Value < MinValue)
      {
        return MinValue;
      }
      if (Value > MaxValue)
      {
        return MaxValue;
      }
      return Value;
    }

    int LZ4AccelerationForLevel(const ESnPakCompressionLevel Level)
    {
      switch (Level)
      {
      case ESnPakCompressionLevel::Fast:
        return 8;
      case ESnPakCompressionLevel::High:
      case ESnPakCompressionLevel::Max:
      case ESnPakCompressionLevel::Default:
      default:
        return 1;
      }
    }

    int LZ4HCLevelForLevel(const ESnPakCompressionLevel Level)
    {
      switch (Level)
      {
      case ESnPakCompressionLevel::Fast:
        return LZ4HC_CLEVEL_MIN;
      case ESnPakCompressionLevel::High:
        return ClampInt(LZ4HC_CLEVEL_DEFAULT + 2, LZ4HC_CLEVEL_MIN, LZ4HC_CLEVEL_MAX);
      case ESnPakCompressionLevel::Max:
        return LZ4HC_CLEVEL_MAX;
      case ESnPakCompressionLevel::Default:
      default:
        return LZ4HC_CLEVEL_DEFAULT;
      }
    }

    int ZstdLevelForMode(ESnPakCompression Mode, ESnPakCompressionLevel Level)
    {
      int Target = ZSTD_defaultCLevel();

      if (Mode == ESnPakCompression::ZstdFast)
      {
        // ZSTD supports negative compression levels for “fast” modes.
        // Clamp to [-ZSTD_maxCLevel(), -1] conservatively.
        const int MinFast = -ZSTD_maxCLevel(); // valid negative range
        constexpr int MaxFast = -1;

        switch (Level)
        {
          case ESnPakCompressionLevel::Fast:    Target = -5; break;
          case ESnPakCompressionLevel::High:    Target = -2; break;
          case ESnPakCompressionLevel::Max:     Target = -1; break;
          case ESnPakCompressionLevel::Default: Target = -3; break;
        }
        return ClampInt(Target, MinFast, MaxFast);
      }
      else
      {
        const int Min = ZSTD_minCLevel();
        const int Max = ZSTD_maxCLevel();
        switch (Level)
        {
          case ESnPakCompressionLevel::Fast:    Target = 1; break;
          case ESnPakCompressionLevel::High:    Target = ZSTD_defaultCLevel() + 5; break;
          case ESnPakCompressionLevel::Max:     Target = Max; break;
          case ESnPakCompressionLevel::Default: Target = ZSTD_defaultCLevel(); break;
        }
        return ClampInt(Target, Min, Max);
      }
    }

    struct ZstdContext
    {
      ZSTD_CCtx* CompressCtx = ZSTD_createCCtx();
      ZSTD_DCtx* DecompressCtx = ZSTD_createDCtx();

      ZstdContext() = default;
      ZstdContext(const ZstdContext&) = delete;
      ZstdContext& operator=(const ZstdContext&) = delete;

      ~ZstdContext()
      {
        if (CompressCtx)
        {
          ZSTD_freeCCtx(CompressCtx);
        }
        if (DecompressCtx)
        {
          ZSTD_freeDCtx(DecompressCtx);
        }
      }
    };

    ZstdContext& GetZstdContext()
    {
      static thread_local ZstdContext Context;
      return Context;
    }

  } // namespace

  std::vector<uint8_t> Compress(const uint8_t* Data, size_t Size, ESnPakCompression Mode, ESnPakCompressionLevel Level)
  {
    if (Mode == ESnPakCompression::None || Size == 0)
    {
      return {Data, Data + Size};
    }

    std::vector<uint8_t> Result;

    if (Mode == ESnPakCompression::LZ4)
    {
      const int MaxCompressedSize = LZ4_compressBound(static_cast<int>(Size));
      Result.resize(MaxCompressedSize);

      const int Acceleration = LZ4AccelerationForLevel(Level);
      const int CompressedSize = LZ4_compress_fast(reinterpret_cast<const char*>(Data), reinterpret_cast<char*>(Result.data()),
                                                    static_cast<int>(Size), MaxCompressedSize, Acceleration);

      if (CompressedSize <= 0)
      {
        throw std::runtime_error("LZ4 compression failed");
      }

      Result.resize(CompressedSize);
    }
    else if (Mode == ESnPakCompression::LZ4HC)
    {
      const int MaxCompressedSize = LZ4_compressBound(static_cast<int>(Size));
      Result.resize(MaxCompressedSize);

      const int LevelValue = LZ4HCLevelForLevel(Level);
      const int CompressedSize = LZ4_compress_HC(reinterpret_cast<const char*>(Data), reinterpret_cast<char*>(Result.data()),
                                                 static_cast<int>(Size), MaxCompressedSize, LevelValue);

      if (CompressedSize <= 0)
      {
        throw std::runtime_error("LZ4HC compression failed");
      }

      Result.resize(CompressedSize);
    }
    else if (Mode == ESnPakCompression::Zstd || Mode == ESnPakCompression::ZstdFast)
    {
      const size_t MaxCompressedSize = ZSTD_compressBound(Size);
      Result.resize(MaxCompressedSize);

      const int LevelValue = ZstdLevelForMode(Mode, Level);
      auto& Context = GetZstdContext();
      const size_t CompressedSize = Context.CompressCtx
                                    ? ZSTD_compressCCtx(Context.CompressCtx, Result.data(), Result.size(), Data, Size, LevelValue)
                                    : ZSTD_compress(Result.data(), Result.size(), Data, Size, LevelValue);

      if (ZSTD_isError(CompressedSize))
      {
        throw std::runtime_error(std::string("Zstd compression failed: ") + ZSTD_getErrorName(CompressedSize));
      }

      Result.resize(CompressedSize);
    }
    else
    {
      throw std::runtime_error("Unknown compression mode");
    }

    return Result;
  }

  std::vector<uint8_t> Compress(const uint8_t* Data, size_t Size, ESnPakCompression Mode)
  {
    return Compress(Data, Size, Mode, ESnPakCompressionLevel::Default);
  }

  std::vector<uint8_t> CompressMax(const uint8_t* Data, size_t Size, ESnPakCompression Mode)
  {
    return Compress(Data, Size, Mode, ESnPakCompressionLevel::Max);
  }

  std::vector<uint8_t> Decompress(const uint8_t* Data, const size_t CompressedSize, const size_t UncompressedSize, ESnPakCompression Mode)
  {
    if (Mode == ESnPakCompression::None)
    {
      return {Data, Data + UncompressedSize};
    }

    std::vector<uint8_t> Result(UncompressedSize);

    if (Mode == ESnPakCompression::LZ4 || Mode == ESnPakCompression::LZ4HC)
    {
      const int DecompressedSize = LZ4_decompress_safe(reinterpret_cast<const char*>(Data), reinterpret_cast<char*>(Result.data()),
                                                       static_cast<int>(CompressedSize), static_cast<int>(UncompressedSize));

      if (DecompressedSize < 0 || static_cast<size_t>(DecompressedSize) != UncompressedSize)
      {
        throw std::runtime_error("LZ4 decompression failed or size mismatch");
      }
    }
    else if (Mode == ESnPakCompression::Zstd || Mode == ESnPakCompression::ZstdFast)
    {
      auto& Context = GetZstdContext();
      const size_t DecompressedSize = Context.DecompressCtx
                                      ? ZSTD_decompressDCtx(Context.DecompressCtx, Result.data(), Result.size(), Data, CompressedSize)
                                      : ZSTD_decompress(Result.data(), Result.size(), Data, CompressedSize);

      if (ZSTD_isError(DecompressedSize))
      {
        throw std::runtime_error(std::string("Zstd decompression failed: ") + ZSTD_getErrorName(DecompressedSize));
      }

      if (DecompressedSize != UncompressedSize)
      {
        throw std::runtime_error("Zstd decompression size mismatch");
      }
    }
    else
    {
      throw std::runtime_error("Unknown compression mode");
    }

    return Result;
  }

} // namespace SnAPI::AssetPipeline::Pack
