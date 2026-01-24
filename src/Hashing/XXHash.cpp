#define XXH_INLINE_ALL
#include <xxhash.h>

// This file exists to ensure xxHash is compiled into the library
// The actual implementation is header-only and included via XXH_INLINE_ALL

namespace AssetPipeline
{
  namespace Hashing
  {

    uint64_t Hash64(const void* Data, std::size_t Size)
    {
      return XXH3_64bits(Data, Size);
    }

    void Hash128(const void* Data, std::size_t Size, uint64_t& OutHi, uint64_t& OutLo)
    {
      XXH128_hash_t Hash = XXH3_128bits(Data, Size);
      OutHi = Hash.high64;
      OutLo = Hash.low64;
    }

    // Streaming hash for large data
    class StreamingHasher64
    {
      public:
        StreamingHasher64()
        {
          m_State = XXH3_createState();
          XXH3_64bits_reset(m_State);
        }

        ~StreamingHasher64()
        {
          XXH3_freeState(m_State);
        }

        void Update(const void* Data, std::size_t Size)
        {
          XXH3_64bits_update(m_State, Data, Size);
        }

        uint64_t Finish()
        {
          return XXH3_64bits_digest(m_State);
        }

        void Reset()
        {
          XXH3_64bits_reset(m_State);
        }

      private:
        XXH3_state_t* m_State;
    };

    class StreamingHasher128
    {
      public:
        StreamingHasher128()
        {
          m_State = XXH3_createState();
          XXH3_128bits_reset(m_State);
        }

        ~StreamingHasher128()
        {
          XXH3_freeState(m_State);
        }

        void Update(const void* Data, std::size_t Size)
        {
          XXH3_128bits_update(m_State, Data, Size);
        }

        void Finish(uint64_t& OutHi, uint64_t& OutLo)
        {
          XXH128_hash_t Hash = XXH3_128bits_digest(m_State);
          OutHi = Hash.high64;
          OutLo = Hash.low64;
        }

        void Reset()
        {
          XXH3_128bits_reset(m_State);
        }

      private:
        XXH3_state_t* m_State;
    };

  } // namespace Hashing
} // namespace AssetPipeline
