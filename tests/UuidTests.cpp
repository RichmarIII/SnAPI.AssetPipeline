#include <catch2/catch_test_macros.hpp>

#include "Uuid.h"

using namespace SnAPI::AssetPipeline;

TEST_CASE("Uuid default construction", "[uuid]")
{
    Uuid Id;
    REQUIRE(Id.IsNull());

    for (int i = 0; i < 16; ++i)
    {
        REQUIRE(Id.Bytes[i] == 0);
    }
}

TEST_CASE("Uuid byte construction", "[uuid]")
{
    Uuid Id(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16);

    REQUIRE_FALSE(Id.IsNull());
    REQUIRE(Id.Bytes[0] == 1);
    REQUIRE(Id.Bytes[15] == 16);
}

TEST_CASE("Uuid equality", "[uuid]")
{
    Uuid A(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16);
    Uuid B(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16);
    Uuid C(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);

    REQUIRE(A == B);
    REQUIRE_FALSE(A == C);
    REQUIRE(A != C);
}

TEST_CASE("Uuid comparison", "[uuid]")
{
    Uuid A(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1);
    Uuid B(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2);

    REQUIRE(A < B);
    REQUIRE_FALSE(B < A);
    REQUIRE_FALSE(A < A);
}

TEST_CASE("Uuid to string", "[uuid]")
{
    Uuid Id(0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0,
            0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88);

    std::string Str = Id.ToString();
    REQUIRE(Str == "12345678-9abc-def0-1122-334455667788");
}

TEST_CASE("Uuid from string", "[uuid]")
{
    Uuid Id = Uuid::FromString("12345678-9abc-def0-1122-334455667788");

    REQUIRE(Id.Bytes[0] == 0x12);
    REQUIRE(Id.Bytes[1] == 0x34);
    REQUIRE(Id.Bytes[2] == 0x56);
    REQUIRE(Id.Bytes[3] == 0x78);
    REQUIRE(Id.Bytes[4] == 0x9a);
    REQUIRE(Id.Bytes[5] == 0xbc);
    REQUIRE(Id.Bytes[6] == 0xde);
    REQUIRE(Id.Bytes[7] == 0xf0);
    REQUIRE(Id.Bytes[8] == 0x11);
    REQUIRE(Id.Bytes[9] == 0x22);
    REQUIRE(Id.Bytes[15] == 0x88);
}

TEST_CASE("Uuid from invalid string returns null", "[uuid]")
{
    Uuid Id = Uuid::FromString("not-a-valid-uuid");
    REQUIRE(Id.IsNull());
}

TEST_CASE("Uuid generate random", "[uuid]")
{
    Uuid A = Uuid::Generate();
    Uuid B = Uuid::Generate();

    REQUIRE_FALSE(A.IsNull());
    REQUIRE_FALSE(B.IsNull());
    REQUIRE(A != B);
}

TEST_CASE("Uuid generate v5 deterministic", "[uuid]")
{
    Uuid Namespace(0x6b, 0xa7, 0xb8, 0x10, 0x9d, 0xad, 0x11, 0xd1,
                   0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8);

    Uuid A = Uuid::GenerateV5(Namespace, "test-asset-name");
    Uuid B = Uuid::GenerateV5(Namespace, "test-asset-name");
    Uuid C = Uuid::GenerateV5(Namespace, "different-name");

    REQUIRE_FALSE(A.IsNull());
    REQUIRE(A == B); // Same input = same output
    REQUIRE(A != C); // Different input = different output
}

TEST_CASE("Uuid hash functor", "[uuid]")
{
    Uuid A(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16);
    Uuid B(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16);
    Uuid C(16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1);

    UuidHash Hasher;

    REQUIRE(Hasher(A) == Hasher(B));
    REQUIRE(Hasher(A) != Hasher(C)); // Highly likely to be different
}

TEST_CASE("Uuid macro", "[uuid]")
{
    constexpr Uuid Id = SNAPI_UUID(0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80,
                                   0x90, 0xa0, 0xb0, 0xc0, 0xd0, 0xe0, 0xf0, 0xff);

    REQUIRE(Id.Bytes[0] == 0x10);
    REQUIRE(Id.Bytes[15] == 0xff);
}
