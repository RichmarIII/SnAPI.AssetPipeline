#include "IPipelineContext.h"
#include "PayloadRegistry.h"

#include <cstdio>
#include <cstdarg>
#include <fstream>
#include <mutex>

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace SnAPI::AssetPipeline
{

  // Well-known namespace UUID for asset IDs (randomly generated once)
  static constexpr Uuid kAssetNamespace = SNAPI_UUID(0x6b, 0xa7, 0xb8, 0x10, 0x9d, 0xad, 0x11, 0xd1, 0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8);

  class PipelineContextImpl : public IPipelineContext
  {
    public:
      PipelineContextImpl(PayloadRegistry* Registry, const std::unordered_map<std::string, std::string>* Options)
          : m_Registry(Registry), m_Options(Options)
      {
      }

      void LogInfo(const char* Fmt, ...) override
      {
        std::lock_guard Lock(m_LogMutex);
        va_list Args;
        va_start(Args, Fmt);
        std::printf("[INFO] ");
        std::vprintf(Fmt, Args);
        std::printf("\n");
        va_end(Args);
      }

      void LogWarn(const char* Fmt, ...) override
      {
        std::lock_guard Lock(m_LogMutex);
        va_list Args;
        va_start(Args, Fmt);
        std::printf("[WARN] ");
        std::vprintf(Fmt, Args);
        std::printf("\n");
        va_end(Args);
      }

      void LogError(const char* Fmt, ...) override
      {
        std::lock_guard Lock(m_LogMutex);
        va_list Args;
        va_start(Args, Fmt);
        std::fprintf(stderr, "[ERROR] ");
        std::vfprintf(stderr, Fmt, Args);
        std::fprintf(stderr, "\n");
        va_end(Args);
      }

      bool ReadAllBytes(const std::string& Uri, std::vector<uint8_t>& Out) override
      {
        std::ifstream File(Uri, std::ios::binary | std::ios::ate);
        if (!File.is_open())
        {
          return false;
        }

        auto Size = File.tellg();
        if (Size <= 0)
        {
          Out.clear();
          return true;
        }

        Out.resize(static_cast<size_t>(Size));
        File.seekg(0, std::ios::beg);
        File.read(reinterpret_cast<char*>(Out.data()), Size);

        return File.good();
      }

      uint64_t HashBytes64(const void* Data, std::size_t Size) override
      {
        return XXH3_64bits(Data, Size);
      }

      void HashBytes128(const void* Data, std::size_t Size, uint64_t& OutHi, uint64_t& OutLo) override
      {
        XXH128_hash_t Hash = XXH3_128bits(Data, Size);
        OutHi = Hash.high64;
        OutLo = Hash.low64;
      }

      AssetId MakeDeterministicAssetId(std::string_view LogicalName, std::string_view VariantKey) override
      {
        std::string Combined;
        Combined.reserve(LogicalName.size() + 1 + VariantKey.size());
        Combined.append(LogicalName);
        Combined.push_back('|');
        Combined.append(VariantKey);

        return Uuid::GenerateV5(kAssetNamespace, Combined);
      }

      const IPayloadSerializer* FindSerializer(TypeId Id) const override
      {
        return m_Registry ? m_Registry->Find(Id) : nullptr;
      }

      std::string GetOption(std::string_view Key, std::string_view Default) const override
      {
        if (m_Options)
        {
          auto It = m_Options->find(std::string(Key));
          if (It != m_Options->end())
          {
            return It->second;
          }
        }
        return std::string(Default);
      }

    private:
      PayloadRegistry* m_Registry = nullptr;
      const std::unordered_map<std::string, std::string>* m_Options = nullptr;
      std::mutex m_LogMutex;
  };

  // Factory function to create a pipeline context
  std::unique_ptr<IPipelineContext> CreatePipelineContext(PayloadRegistry* Registry, const std::unordered_map<std::string, std::string>* Options)
  {
    return std::make_unique<PipelineContextImpl>(Registry, Options);
  }

} // namespace SnAPI::AssetPipeline
