#include <catch2/catch_test_macros.hpp>

#include "AssetPipeline.h"
#include "PayloadRegistry.h"
#include "TypedPayload.h"
#include "IPayloadSerializer.h"

#include <vector>
#include <sstream>

using namespace SnAPI::AssetPipeline;

// Simple test serializer
class TestPayloadSerializer : public IPayloadSerializer
{
public:
    TestPayloadSerializer(TypeId Id, const char* Name, uint32_t Version)
        : m_Id(Id), m_Name(Name), m_Version(Version) {}

    TypeId GetTypeId() const override { return m_Id; }
    const char* GetTypeName() const override { return m_Name; }
    uint32_t GetSchemaVersion() const override { return m_Version; }

    void SerializeToBytes(const void* Object, std::vector<uint8_t>& OutBytes) const override
    {
        // Simple test: just copy raw bytes
        const auto* Data = static_cast<const std::vector<uint8_t>*>(Object);
        OutBytes = *Data;
    }

    bool DeserializeFromBytes(void* Object, const uint8_t* Bytes, std::size_t Size) const override
    {
        auto* Data = static_cast<std::vector<uint8_t>*>(Object);
        Data->assign(Bytes, Bytes + Size);
        return true;
    }

private:
    TypeId m_Id;
    const char* m_Name;
    uint32_t m_Version;
};

TEST_CASE("PayloadRegistry basic operations", "[pipeline]")
{
    PayloadRegistry Registry;

    TypeId TestType = SNAPI_UUID(0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10);

    REQUIRE_FALSE(Registry.IsFrozen());

    auto Serializer = std::make_unique<TestPayloadSerializer>(TestType, "TestType", 1);
    Registry.Register(std::move(Serializer));

    // Should find it before freeze
    const IPayloadSerializer* Found = Registry.Find(TestType);
    REQUIRE(Found != nullptr);
    REQUIRE(Found->GetSchemaVersion() == 1);
    REQUIRE(std::string(Found->GetTypeName()) == "TestType");

    // Freeze
    Registry.Freeze();
    REQUIRE(Registry.IsFrozen());

    // Should still find it after freeze
    Found = Registry.Find(TestType);
    REQUIRE(Found != nullptr);

    // Registration after freeze should throw
    TypeId AnotherType = SNAPI_UUID(0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
                                    0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20);
    REQUIRE_THROWS(Registry.Register(
        std::make_unique<TestPayloadSerializer>(AnotherType, "AnotherType", 1)));
}

TEST_CASE("PayloadRegistry find by name", "[pipeline]")
{
    PayloadRegistry Registry;

    TypeId TestType = SNAPI_UUID(0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10);

    Registry.Register(std::make_unique<TestPayloadSerializer>(TestType, "MyTestType", 2));

    const IPayloadSerializer* Found = Registry.FindByName("MyTestType");
    REQUIRE(Found != nullptr);
    REQUIRE(Found->GetSchemaVersion() == 2);

    Found = Registry.FindByName("NonExistent");
    REQUIRE(Found == nullptr);
}

TEST_CASE("PayloadRegistry duplicate type rejection", "[pipeline]")
{
    PayloadRegistry Registry;

    TypeId TestType = SNAPI_UUID(0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10);

    Registry.Register(std::make_unique<TestPayloadSerializer>(TestType, "Type1", 1));

    // Same TypeId should throw
    REQUIRE_THROWS(Registry.Register(
        std::make_unique<TestPayloadSerializer>(TestType, "Type2", 1)));
}

TEST_CASE("PayloadRegistry duplicate name rejection", "[pipeline]")
{
    PayloadRegistry Registry;

    TypeId Type1 = SNAPI_UUID(0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                              0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10);
    TypeId Type2 = SNAPI_UUID(0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
                              0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20);

    Registry.Register(std::make_unique<TestPayloadSerializer>(Type1, "SameName", 1));

    // Same name should throw
    REQUIRE_THROWS(Registry.Register(
        std::make_unique<TestPayloadSerializer>(Type2, "SameName", 1)));
}

TEST_CASE("PayloadRegistry GetAll", "[pipeline]")
{
    PayloadRegistry Registry;

    TypeId Type1 = SNAPI_UUID(0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                              0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10);
    TypeId Type2 = SNAPI_UUID(0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
                              0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20);

    Registry.Register(std::make_unique<TestPayloadSerializer>(Type1, "Type1", 1));
    Registry.Register(std::make_unique<TestPayloadSerializer>(Type2, "Type2", 1));

    const auto& All = Registry.GetAll();
    REQUIRE(All.size() == 2);
}

TEST_CASE("TypedPayload basic operations", "[pipeline]")
{
    TypedPayload Payload;

    REQUIRE(Payload.IsEmpty());
    REQUIRE(Payload.SchemaVersion == 0);
    REQUIRE(Payload.PayloadType.IsNull());

    TypeId TestType = SNAPI_UUID(0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10);

    Payload.PayloadType = TestType;
    Payload.SchemaVersion = 3;
    Payload.Bytes = {1, 2, 3, 4, 5};

    REQUIRE_FALSE(Payload.IsEmpty());
    REQUIRE(Payload.SchemaVersion == 3);
    REQUIRE(Payload.Bytes.size() == 5);

    Payload.Clear();
    REQUIRE(Payload.IsEmpty());
    REQUIRE(Payload.PayloadType.IsNull());
}

TEST_CASE("TypedPayload constructor", "[pipeline]")
{
    TypeId TestType = SNAPI_UUID(0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10);

    TypedPayload Payload(TestType, 5, {10, 20, 30});

    REQUIRE(Payload.PayloadType == TestType);
    REQUIRE(Payload.SchemaVersion == 5);
    REQUIRE(Payload.Bytes.size() == 3);
    REQUIRE(Payload.Bytes[0] == 10);
}

TEST_CASE("PipelineBuildConfig defaults", "[pipeline]")
{
    PipelineBuildConfig Config;

    REQUIRE(Config.SourceRoots.empty());
    REQUIRE(Config.PluginPaths.empty());
    REQUIRE(Config.OutputPackPath.empty());
    REQUIRE(Config.bDeterministicAssetIds == true);
    REQUIRE(Config.bEnableAppendUpdates == true);
    REQUIRE(Config.Compression == EPackCompression::Zstd);
    REQUIRE(Config.CompressionLevel == EPackCompressionLevel::Default);
    REQUIRE(Config.ParallelJobs == 0);
    REQUIRE(Config.bVerbose == false);
}

TEST_CASE("AssetPipelineEngine initialization with empty config succeeds", "[pipeline]")
{
    AssetPipelineEngine Engine;

    PipelineBuildConfig Config;
    // Empty config is valid (relaxed: no source roots/output required)
    auto Result = Engine.Initialize(Config);
    REQUIRE(Result.has_value());
}

TEST_CASE("AssetPipelineEngine initialization fails with nonexistent source root", "[pipeline]")
{
    AssetPipelineEngine Engine;

    PipelineBuildConfig Config;
    Config.SourceRoots = {"/nonexistent/path/xyz"};
    auto Result = Engine.Initialize(Config);
    REQUIRE_FALSE(Result.has_value());
}
