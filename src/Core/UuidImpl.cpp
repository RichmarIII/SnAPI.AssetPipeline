#include "Uuid.h"

#include <uuid.h>
#include <iomanip>
#include <sstream>
#include <random>

namespace SnAPI::AssetPipeline
{

  std::string Uuid::ToString() const
  {
    std::ostringstream Oss;
    Oss << std::hex << std::setfill('0');

    for (int i = 0; i < 16; ++i)
    {
      if (i == 4 || i == 6 || i == 8 || i == 10)
      {
        Oss << '-';
      }
      Oss << std::setw(2) << static_cast<int>(Bytes[i]);
    }

    return Oss.str();
  }

  Uuid Uuid::FromString(const std::string& Str)
  {
    Uuid Result;

    try
    {
      auto StdUuid = uuids::uuid::from_string(Str);
      if (StdUuid.has_value())
      {
        auto Span = StdUuid->as_bytes();
        for (int i = 0; i < 16; ++i)
        {
          Result.Bytes[i] = static_cast<uint8_t>(Span[i]);
        }
      }
    }
    catch (...)
    {
      // Return null UUID on parse failure
    }

    return Result;
  }

  Uuid Uuid::Generate()
  {
    Uuid Result;

    std::random_device Rd;
    std::mt19937 Engine(Rd());
    uuids::uuid_random_generator Gen(Engine);
    auto StdUuid = Gen();

    auto Span = StdUuid.as_bytes();
    for (int i = 0; i < 16; ++i)
    {
      Result.Bytes[i] = static_cast<uint8_t>(Span[i]);
    }

    return Result;
  }

  Uuid Uuid::GenerateV5(const Uuid& Namespace, const std::string& Name)
  {
    Uuid Result;

    // Convert our namespace to stduuid format
    std::array<uint8_t, 16> NsBytes;
    for (int i = 0; i < 16; ++i)
    {
      NsBytes[i] = Namespace.Bytes[i];
    }
    uuids::uuid NsUuid(NsBytes);

    // Generate v5 UUID
    uuids::uuid_name_generator Gen(NsUuid);
    auto StdUuid = Gen(Name);

    auto Span = StdUuid.as_bytes();
    for (int i = 0; i < 16; ++i)
    {
      Result.Bytes[i] = static_cast<uint8_t>(Span[i]);
    }

    return Result;
  }

} // namespace SnAPI::AssetPipeline
