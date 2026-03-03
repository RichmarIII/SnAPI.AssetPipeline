#include <catch2/catch_test_macros.hpp>

#include "AssetManager.h"
#include "AssetPackReader.h"
#include "AssetPackWriter.h"
#include "Pack/SnPakFormat.h"
#include "SourceMountConfig.h"
#include "Runtime/SourceAssetResolver.h"
#include "Runtime/AutoMountScanner.h"
#include "IAssetImporter.h"
#include "IAssetCooker.h"
#include "IPipelineContext.h"
#include "IPayloadSerializer.h"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>

using namespace SnAPI::AssetPipeline;

namespace
{

  // Helper to create a temporary directory
  struct TempDir
  {
      std::filesystem::path Path;

      TempDir()
      {
        Path = std::filesystem::temp_directory_path() / ("snapi_test_" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) +
                                                         "_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(Path);
      }

      ~TempDir()
      {
        std::filesystem::remove_all(Path);
      }
  };

  void WriteFile(const std::filesystem::path& FilePath, const std::string& Content)
  {
    std::filesystem::create_directories(FilePath.parent_path());
    std::ofstream File(FilePath, std::ios::binary);
    File.write(Content.data(), static_cast<std::streamsize>(Content.size()));
  }

  struct PackIndexSnapshot
  {
      Pack::SnPakHeaderV1 Header{};
      std::vector<Pack::SnPakIndexEntryV1> Entries{};
      std::vector<Pack::SnPakBulkEntryV1> BulkEntries{};
  };

  std::optional<PackIndexSnapshot> ReadActiveIndexSnapshot(const std::filesystem::path& PackPath)
  {
    std::ifstream File(PackPath, std::ios::binary);
    if (!File.is_open())
    {
      return std::nullopt;
    }

    PackIndexSnapshot Snapshot{};
    File.read(reinterpret_cast<char*>(&Snapshot.Header), sizeof(Snapshot.Header));
    if (!File.good())
    {
      return std::nullopt;
    }
    if (std::memcmp(Snapshot.Header.Magic, Pack::kSnPakMagic, 8) != 0)
    {
      return std::nullopt;
    }

    File.seekg(static_cast<std::streamoff>(Snapshot.Header.IndexOffset), std::ios::beg);
    if (!File.good())
    {
      return std::nullopt;
    }

    Pack::SnPakIndexHeaderV1 IndexHeader{};
    File.read(reinterpret_cast<char*>(&IndexHeader), sizeof(IndexHeader));
    if (!File.good())
    {
      return std::nullopt;
    }
    if (std::memcmp(IndexHeader.Magic, Pack::kIndexMagic, 4) != 0)
    {
      return std::nullopt;
    }

    Snapshot.Entries.resize(IndexHeader.EntryCount);
    if (!Snapshot.Entries.empty())
    {
      File.read(reinterpret_cast<char*>(Snapshot.Entries.data()),
                static_cast<std::streamsize>(Snapshot.Entries.size() * sizeof(Pack::SnPakIndexEntryV1)));
      if (!File.good())
      {
        return std::nullopt;
      }
    }

    Snapshot.BulkEntries.resize(IndexHeader.BulkEntryCount);
    if (!Snapshot.BulkEntries.empty())
    {
      File.read(reinterpret_cast<char*>(Snapshot.BulkEntries.data()),
                static_cast<std::streamsize>(Snapshot.BulkEntries.size() * sizeof(Pack::SnPakBulkEntryV1)));
      if (!File.good())
      {
        return std::nullopt;
      }
    }

    return Snapshot;
  }

  const Pack::SnPakIndexEntryV1* FindIndexEntryByAssetId(const PackIndexSnapshot& Snapshot, const AssetId& Asset)
  {
    for (const auto& Entry : Snapshot.Entries)
    {
      if (std::memcmp(Entry.AssetId, Asset.Bytes, sizeof(Asset.Bytes)) == 0)
      {
        return &Entry;
      }
    }
    return nullptr;
  }

} // namespace

// ========== SourceAssetResolver Tests ==========

TEST_CASE("SourceAssetResolver resolves files from a single root", "[source]")
{
  TempDir Dir;
  WriteFile(Dir.Path / "textures" / "hero.png", "PNG_DATA");
  WriteFile(Dir.Path / "models" / "enemy.obj", "OBJ_DATA");

  SourceAssetResolver Resolver;
  SourceMountConfig Config;
  Config.RootPath = Dir.Path.string();
  Resolver.AddRoot(Config);

  SECTION("resolves existing file")
  {
    auto Result = Resolver.Resolve("textures/hero.png");
    REQUIRE(Result.has_value());
    REQUIRE(Result->LogicalName == "textures/hero.png");
    REQUIRE(std::filesystem::exists(Result->AbsolutePath));
  }

  SECTION("resolves another existing file")
  {
    auto Result = Resolver.Resolve("models/enemy.obj");
    REQUIRE(Result.has_value());
    REQUIRE(Result->LogicalName == "models/enemy.obj");
  }

  SECTION("returns nullopt for non-existent file")
  {
    auto Result = Resolver.Resolve("textures/missing.png");
    REQUIRE_FALSE(Result.has_value());
  }
}

TEST_CASE("SourceAssetResolver respects mount points", "[source]")
{
  TempDir Dir;
  WriteFile(Dir.Path / "hero.png", "PNG_DATA");

  SourceAssetResolver Resolver;
  SourceMountConfig Config;
  Config.RootPath = Dir.Path.string();
  Config.MountPoint = "game/textures/";
  Resolver.AddRoot(Config);

  SECTION("resolves with matching mount point prefix")
  {
    auto Result = Resolver.Resolve("game/textures/hero.png");
    REQUIRE(Result.has_value());
    REQUIRE(Result->LogicalName == "game/textures/hero.png");
  }

  SECTION("does not resolve without mount point prefix")
  {
    auto Result = Resolver.Resolve("hero.png");
    REQUIRE_FALSE(Result.has_value());
  }
}

TEST_CASE("SourceAssetResolver respects priority order", "[source]")
{
  TempDir Dir1;
  TempDir Dir2;
  WriteFile(Dir1.Path / "shared.txt", "FROM_DIR1");
  WriteFile(Dir2.Path / "shared.txt", "FROM_DIR2");

  SourceAssetResolver Resolver;

  SourceMountConfig Config1;
  Config1.RootPath = Dir1.Path.string();
  Config1.Priority = 10;

  SourceMountConfig Config2;
  Config2.RootPath = Dir2.Path.string();
  Config2.Priority = 20; // Higher priority

  Resolver.AddRoot(Config1);
  Resolver.AddRoot(Config2);

  auto Result = Resolver.Resolve("shared.txt");
  REQUIRE(Result.has_value());
  // Should resolve to Dir2 (higher priority)
  REQUIRE(Result->AbsolutePath == (Dir2.Path / "shared.txt").string());
}

TEST_CASE("SourceAssetResolver RemoveRoot", "[source]")
{
  TempDir Dir;
  WriteFile(Dir.Path / "file.txt", "DATA");

  SourceAssetResolver Resolver;
  SourceMountConfig Config;
  Config.RootPath = Dir.Path.string();
  Resolver.AddRoot(Config);

  REQUIRE(Resolver.Resolve("file.txt").has_value());

  Resolver.RemoveRoot(Dir.Path.string());

  REQUIRE_FALSE(Resolver.Resolve("file.txt").has_value());
}

// ========== AutoMountScanner Tests ==========

TEST_CASE("AutoMountScanner discovers .snpak files", "[source]")
{
  TempDir Dir;
  WriteFile(Dir.Path / "pack1.snpak", "PACK1");
  WriteFile(Dir.Path / "sub" / "pack2.snpak", "PACK2");
  WriteFile(Dir.Path / "not_a_pack.txt", "TEXT");

  auto Results = AutoMountScanner::Scan({Dir.Path.string()});

  REQUIRE(Results.size() == 2);

  // Check both snpak files are found
  bool bFoundPack1 = false;
  bool bFoundPack2 = false;
  for (const auto& Path : Results)
  {
    if (Path.find("pack1.snpak") != std::string::npos)
      bFoundPack1 = true;
    if (Path.find("pack2.snpak") != std::string::npos)
      bFoundPack2 = true;
  }
  REQUIRE(bFoundPack1);
  REQUIRE(bFoundPack2);
}

TEST_CASE("AutoMountScanner handles non-existent directories gracefully", "[source]")
{
  auto Results = AutoMountScanner::Scan({"/non/existent/path/12345"});
  REQUIRE(Results.empty());
}

TEST_CASE("AutoMountScanner scans multiple directories", "[source]")
{
  TempDir Dir1;
  TempDir Dir2;
  WriteFile(Dir1.Path / "a.snpak", "A");
  WriteFile(Dir2.Path / "b.snpak", "B");

  auto Results = AutoMountScanner::Scan({Dir1.Path.string(), Dir2.Path.string()});
  REQUIRE(Results.size() == 2);
}

// ========== AssetManager Source Asset Integration Tests ==========

TEST_CASE("AssetManager config with source assets disabled does not create resolver", "[source]")
{
  AssetManagerConfig Config;
  Config.bEnableSourceAssets = false;

  AssetManager Manager(Config);

  // Should not crash and basic operations should work
  auto Result = Manager.FindAsset("nonexistent.png");
  REQUIRE_FALSE(Result.has_value());
}

TEST_CASE("AssetManager auto-mounts packs from search paths", "[source]")
{
  TempDir Dir;
  // Create a dummy file that won't be a valid snpak but tests the scanning
  WriteFile(Dir.Path / "test.snpak", "NOT_VALID_SNPAK");

  AssetManagerConfig Config;
  Config.PackSearchPaths = {Dir.Path.string()};

  // This will try to mount the pack (which will fail since it's not a valid snpak)
  // but should not crash
  AssetManager Manager(Config);

  // The invalid pack won't be mounted
  auto Packs = Manager.GetMountedPacks();
  REQUIRE(Packs.empty());
}

TEST_CASE("AssetManager AddSourceRoot and RemoveSourceRoot", "[source]")
{
  TempDir Dir;
  WriteFile(Dir.Path / "test.txt", "DATA");

  AssetManagerConfig Config;
  Config.bEnableSourceAssets = true;

  AssetManager Manager(Config);

  SourceMountConfig MountConfig;
  MountConfig.RootPath = Dir.Path.string();
  Manager.AddSourceRoot(MountConfig);

  Manager.RemoveSourceRoot(Dir.Path.string());
}

TEST_CASE("AssetManager GetDirtyAssetCount returns 0 when pipeline disabled", "[source]")
{
  AssetManagerConfig Config;
  AssetManager Manager(Config);
  REQUIRE(Manager.GetDirtyAssetCount() == 0);
}

TEST_CASE("AssetManager SaveRuntimeAssets succeeds with no dirty assets", "[source]")
{
  AssetManagerConfig Config;
  AssetManager Manager(Config);
  auto Result = Manager.SaveRuntimeAssets();
  REQUIRE(Result.has_value()); // No dirty assets = nothing to save = success
  REQUIRE(Manager.GetDirtyAssetCount() == 0);
}

TEST_CASE("AssetPackWriter AppendUpdate preserves existing assets and untouched bulk", "[source][pack]")
{
  TempDir Dir;
  const std::string PackPath = (Dir.Path / "append_preserve.snpak").string();

  const AssetId AssetA = AssetId::Generate();
  const AssetId AssetB = AssetId::Generate();
  const TypeId AssetKind = SNAPI_UUID(0x44, 0x12, 0x90, 0x71, 0x88, 0x66, 0x55, 0x11, 0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE);
  const TypeId PayloadType = SNAPI_UUID(0x55, 0x23, 0x91, 0x72, 0x89, 0x67, 0x56, 0x12, 0x11, 0x33, 0x55, 0x77, 0x99, 0xBB, 0xDD, 0xFF);

  const auto MakePayload = [&PayloadType](std::string_view Text) {
    return TypedPayload(PayloadType, 1u, std::vector<uint8_t>(Text.begin(), Text.end()));
  };

  std::vector<BulkChunk> AssetABulk{};
  {
    BulkChunk Chunk(EBulkSemantic::Unknown, 0u, false);
    const std::string BulkText = "ASSET_A_BULK";
    Chunk.Bytes.assign(BulkText.begin(), BulkText.end());
    AssetABulk.push_back(std::move(Chunk));
  }

  AssetPackWriter InitialWriter{};
  InitialWriter.AddAsset(AssetA, AssetKind, "AssetA", "", MakePayload("A_v1"), AssetABulk);
  InitialWriter.AddAsset(AssetB, AssetKind, "AssetB", "", MakePayload("B_v1"), {});
  REQUIRE(InitialWriter.Write(PackPath).has_value());

  const auto BeforeSnapshot = ReadActiveIndexSnapshot(PackPath);
  REQUIRE(BeforeSnapshot.has_value());
  const Pack::SnPakIndexEntryV1* const BeforeAssetAEntry = FindIndexEntryByAssetId(*BeforeSnapshot, AssetA);
  const Pack::SnPakIndexEntryV1* const BeforeAssetBEntry = FindIndexEntryByAssetId(*BeforeSnapshot, AssetB);
  REQUIRE(BeforeAssetAEntry != nullptr);
  REQUIRE(BeforeAssetBEntry != nullptr);
  REQUIRE((BeforeAssetAEntry->Flags & Pack::IndexEntryFlag_HasBulk) != 0);
  REQUIRE(BeforeAssetAEntry->BulkCount == 1u);
  REQUIRE(BeforeAssetAEntry->BulkFirstIndex < BeforeSnapshot->BulkEntries.size());
  const uint64_t BeforeAssetABulkOffset = BeforeSnapshot->BulkEntries[BeforeAssetAEntry->BulkFirstIndex].ChunkOffset;
  const uint64_t BeforeAssetBPayloadOffset = BeforeAssetBEntry->PayloadChunkOffset;
  const uint64_t BeforeAssetBPayloadSizeCompressed = BeforeAssetBEntry->PayloadChunkSizeCompressed;

  // Update only asset A cooked bytes; no bulk is provided in the update.
  AssetPackWriter UpdateWriter{};
  UpdateWriter.AddAsset(AssetA, AssetKind, "AssetA", "", MakePayload("A_v2"), {});
  REQUIRE(UpdateWriter.AppendUpdate(PackPath).has_value());

  const auto AfterSnapshot = ReadActiveIndexSnapshot(PackPath);
  REQUIRE(AfterSnapshot.has_value());
  const Pack::SnPakIndexEntryV1* const AfterAssetAEntry = FindIndexEntryByAssetId(*AfterSnapshot, AssetA);
  const Pack::SnPakIndexEntryV1* const AfterAssetBEntry = FindIndexEntryByAssetId(*AfterSnapshot, AssetB);
  REQUIRE(AfterAssetAEntry != nullptr);
  REQUIRE(AfterAssetBEntry != nullptr);
  REQUIRE(AfterAssetBEntry->PayloadChunkOffset == BeforeAssetBPayloadOffset);
  REQUIRE(AfterAssetBEntry->PayloadChunkSizeCompressed == BeforeAssetBPayloadSizeCompressed);
  REQUIRE((AfterAssetAEntry->Flags & Pack::IndexEntryFlag_HasBulk) != 0);
  REQUIRE(AfterAssetAEntry->BulkCount == 1u);
  REQUIRE(AfterAssetAEntry->BulkFirstIndex < AfterSnapshot->BulkEntries.size());
  REQUIRE(AfterSnapshot->BulkEntries[AfterAssetAEntry->BulkFirstIndex].ChunkOffset == BeforeAssetABulkOffset);

  AssetPackReader Reader{};
  REQUIRE(Reader.Open(PackPath).has_value());
  REQUIRE(Reader.GetAssetCount() == 2u);

  auto AssetACooked = Reader.LoadCookedPayload(AssetA);
  REQUIRE(AssetACooked.has_value());
  REQUIRE(std::string(AssetACooked->Bytes.begin(), AssetACooked->Bytes.end()) == "A_v2");

  auto AssetBCooked = Reader.LoadCookedPayload(AssetB);
  REQUIRE(AssetBCooked.has_value());
  REQUIRE(std::string(AssetBCooked->Bytes.begin(), AssetBCooked->Bytes.end()) == "B_v1");

  auto AssetAInfo = Reader.FindAsset(AssetA);
  REQUIRE(AssetAInfo.has_value());
  REQUIRE(AssetAInfo->BulkChunkCount == 1u);

  auto AssetABulkRead = Reader.LoadBulkChunk(AssetA, 0u);
  REQUIRE(AssetABulkRead.has_value());
  REQUIRE(std::string(AssetABulkRead->begin(), AssetABulkRead->end()) == "ASSET_A_BULK");
}

// ========== Runtime Pipeline Integration Tests ==========

namespace
{

  // Test TypeIds
  const TypeId kTestAssetKind{0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0, 0xF0, 0x01};
  const TypeId kTestIntermediateType{0x11, 0x21, 0x31, 0x41, 0x51, 0x61, 0x71, 0x81, 0x91, 0xA1, 0xB1, 0xC1, 0xD1, 0xE1, 0xF1, 0x02};
  const TypeId kTestCookedType{0x12, 0x22, 0x32, 0x42, 0x52, 0x62, 0x72, 0x82, 0x92, 0xA2, 0xB2, 0xC2, 0xD2, 0xE2, 0xF2, 0x03};

  class MockBytesPayloadSerializer : public IPayloadSerializer
  {
    public:
      explicit MockBytesPayloadSerializer(const TypeId Type, const char* Name, const uint32_t SchemaVersion = 1u)
          : m_Type(Type), m_Name(Name), m_SchemaVersion(SchemaVersion)
      {
      }

      TypeId GetTypeId() const override
      {
        return m_Type;
      }

      const char* GetTypeName() const override
      {
        return m_Name;
      }

      uint32_t GetSchemaVersion() const override
      {
        return m_SchemaVersion;
      }

      void SerializeToBytes(const void* Object, std::vector<uint8_t>& OutBytes) const override
      {
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
      TypeId m_Type{};
      const char* m_Name = "";
      uint32_t m_SchemaVersion = 1u;
  };

  // Simple runtime type representing a processed asset
  struct TestRuntimeObject
  {
      std::string OriginalContent;
      uint32_t ProcessedLength = 0;
  };

  // Mock importer: reads ".testasset" files and produces an intermediate payload
  class MockImporter : public IAssetImporter
  {
    public:
      std::atomic<int> ImportCount{0};

      const char* GetName() const override
      {
        return "MockImporter";
      }

      bool CanImport(const SourceRef& Source) const override
      {
        return Source.Uri.ends_with(".testasset");
      }

      bool Import(const SourceRef& Source, std::vector<ImportedItem>& OutItems, IPipelineContext& Ctx) override
      {
        ++ImportCount;

        // Read the source file content
        std::vector<uint8_t> FileData;
        if (!Ctx.ReadAllBytes(Source.Uri, FileData))
        {
          return false;
        }

        ImportedItem Item;
        Item.Id = Uuid::Generate();
        Item.AssetKind = kTestAssetKind;
        Item.Intermediate = TypedPayload(kTestIntermediateType, 1, std::move(FileData));

        OutItems.push_back(std::move(Item));
        return true;
      }
  };

  // Mock cooker: transforms intermediate → cooked payload (prefixes with "COOKED:")
  class MockCooker : public IAssetCooker
  {
    public:
      std::atomic<int> CookCount{0};

      const char* GetName() const override
      {
        return "MockCooker";
      }

      bool CanCook(TypeId AssetKind, TypeId IntermediatePayloadType) const override
      {
        return AssetKind == kTestAssetKind && IntermediatePayloadType == kTestIntermediateType;
      }

      bool Cook(const CookRequest& Req, CookResult& Out, IPipelineContext& Ctx) override
      {
        ++CookCount;

        // Transform: prepend "COOKED:" to the intermediate bytes
        std::string Prefix = "COOKED:";
        std::vector<uint8_t> CookedBytes;
        CookedBytes.insert(CookedBytes.end(), Prefix.begin(), Prefix.end());
        CookedBytes.insert(CookedBytes.end(), Req.Intermediate.Bytes.begin(), Req.Intermediate.Bytes.end());

        Out.Cooked = TypedPayload(kTestCookedType, 1, std::move(CookedBytes));

        // Add a bulk chunk for testing bulk access
        BulkChunk Chunk(EBulkSemantic::Unknown, 0, false);
        std::string BulkData = "BULK_DATA_FOR_" + Req.LogicalName;
        Chunk.Bytes.assign(BulkData.begin(), BulkData.end());
        Out.Bulk.push_back(std::move(Chunk));

        return true;
      }
  };

  // Mock factory: creates TestRuntimeObject from cooked payload
  class MockFactory : public TAssetFactory<TestRuntimeObject>
  {
    public:
      TypeId GetCookedPayloadType() const override
      {
        return kTestCookedType;
      }

    protected:
      std::expected<TestRuntimeObject, std::string> DoLoad(const AssetLoadContext& Context) override
      {
        TestRuntimeObject Obj;
        Obj.OriginalContent = std::string(Context.Cooked.Bytes.begin(), Context.Cooked.Bytes.end());
        Obj.ProcessedLength = static_cast<uint32_t>(Context.Cooked.Bytes.size());
        return Obj;
      }
  };

  // Helper to set up AssetManager with source pipeline support
  struct PipelineTestFixture
  {
      TempDir SourceDir;
      TempDir OutputDir;
      MockImporter* ImporterPtr = nullptr;
      MockCooker* CookerPtr = nullptr;

      std::unique_ptr<AssetManager> CreateManager()
      {
        AssetManagerConfig Config;
        Config.bEnableSourceAssets = true;

        SourceMountConfig SourceMount;
        SourceMount.RootPath = SourceDir.Path.string();
        Config.SourceRoots.push_back(SourceMount);

        Config.PipelineConfig.OutputPackPath = (OutputDir.Path / "test_runtime.snpak").string();
        Config.PipelineConfig.bDeterministicAssetIds = true;

        auto Manager = std::make_unique<AssetManager>(Config);

        Manager->RegisterSerializer(std::make_unique<MockBytesPayloadSerializer>(kTestCookedType, "MockCookedPayload"));

        // Register mock importer & cooker
        auto Importer = std::make_unique<MockImporter>();
        ImporterPtr = Importer.get();
        Manager->RegisterImporter(std::move(Importer));

        auto Cooker = std::make_unique<MockCooker>();
        CookerPtr = Cooker.get();
        Manager->RegisterCooker(std::move(Cooker));

        // Register factory
        Manager->RegisterFactory<TestRuntimeObject>(std::make_unique<MockFactory>());

        return Manager;
      }
  };

} // namespace

TEST_CASE("AssetManager loads source asset through runtime pipeline", "[source][pipeline]")
{
  PipelineTestFixture Fixture;
  WriteFile(Fixture.SourceDir.Path / "test.testasset", "HELLO_WORLD");

  auto Manager = Fixture.CreateManager();

  auto Result = Manager->Load<TestRuntimeObject>("test.testasset");
  REQUIRE(Result.has_value());
  REQUIRE(Result->get()->OriginalContent == "COOKED:HELLO_WORLD");
  REQUIRE(Result->get()->ProcessedLength == 18); // "COOKED:" (7) + "HELLO_WORLD" (11)
  REQUIRE(Fixture.ImporterPtr->ImportCount == 1);
  REQUIRE(Fixture.CookerPtr->CookCount == 1);
}

TEST_CASE("AssetManager does not re-pipeline already processed source", "[source][pipeline]")
{
  PipelineTestFixture Fixture;
  WriteFile(Fixture.SourceDir.Path / "cached.testasset", "DATA");

  auto Manager = Fixture.CreateManager();

  // First load - pipelines the source
  auto Result1 = Manager->Load<TestRuntimeObject>("cached.testasset");
  REQUIRE(Result1.has_value());
  REQUIRE(Fixture.ImporterPtr->ImportCount == 1);
  REQUIRE(Fixture.CookerPtr->CookCount == 1);

  // Second load - should reuse in-memory cooked data (no re-import/re-cook)
  auto Result2 = Manager->Load<TestRuntimeObject>("cached.testasset");
  REQUIRE(Result2.has_value());
  REQUIRE(Fixture.ImporterPtr->ImportCount == 1); // Still 1 - not re-imported
  REQUIRE(Fixture.CookerPtr->CookCount == 1);     // Still 1 - not re-cooked

  // Both loads should produce the same content
  REQUIRE(Result1->get()->OriginalContent == Result2->get()->OriginalContent);
}

TEST_CASE("AssetManager pipeline tracks dirty assets", "[source][pipeline]")
{
  PipelineTestFixture Fixture;
  WriteFile(Fixture.SourceDir.Path / "dirty.testasset", "DIRTY_DATA");

  auto Manager = Fixture.CreateManager();
  REQUIRE(Manager->GetDirtyAssetCount() == 0);

  auto Result = Manager->Load<TestRuntimeObject>("dirty.testasset");
  REQUIRE(Result.has_value());
  REQUIRE(Manager->GetDirtyAssetCount() == 1);
}

TEST_CASE("AssetManager SaveRuntimeAssets persists pipelined assets", "[source][pipeline]")
{
  PipelineTestFixture Fixture;
  WriteFile(Fixture.SourceDir.Path / "persist.testasset", "PERSIST_ME");

  auto Manager = Fixture.CreateManager();

  // Pipeline the asset
  auto Result = Manager->Load<TestRuntimeObject>("persist.testasset");
  REQUIRE(Result.has_value());
  REQUIRE(Manager->GetDirtyAssetCount() == 1);

  // Save to disk
  auto SaveResult = Manager->SaveRuntimeAssets();
  REQUIRE(SaveResult.has_value());
  REQUIRE(Manager->GetDirtyAssetCount() == 0);

  // Verify the runtime pack file was created
  auto PackPath = Fixture.OutputDir.Path / "test_runtime.snpak";
  REQUIRE(std::filesystem::exists(PackPath));
}

TEST_CASE("AssetManager pipeline returns error for missing source file", "[source][pipeline]")
{
  PipelineTestFixture Fixture;
  // No source file created

  auto Manager = Fixture.CreateManager();

  auto Result = Manager->Load<TestRuntimeObject>("nonexistent.testasset");
  REQUIRE_FALSE(Result.has_value());
  REQUIRE(Result.error().find("not found") != std::string::npos);
}

TEST_CASE("AssetManager pipeline returns error for unsupported extension", "[source][pipeline]")
{
  PipelineTestFixture Fixture;
  WriteFile(Fixture.SourceDir.Path / "file.unknown", "DATA");

  auto Manager = Fixture.CreateManager();

  auto Result = Manager->Load<TestRuntimeObject>("file.unknown");
  REQUIRE_FALSE(Result.has_value());
  // Should fail because no importer handles ".unknown"
}

TEST_CASE("AssetManager pipeline with subdirectory source path", "[source][pipeline]")
{
  PipelineTestFixture Fixture;
  WriteFile(Fixture.SourceDir.Path / "textures" / "hero.testasset", "HERO_DATA");

  auto Manager = Fixture.CreateManager();

  auto Result = Manager->Load<TestRuntimeObject>("textures/hero.testasset");
  REQUIRE(Result.has_value());
  REQUIRE(Result->get()->OriginalContent == "COOKED:HERO_DATA");
}

TEST_CASE("AssetManager pipeline with bulk chunks accessible", "[source][pipeline]")
{
  // This test verifies that bulk chunks from the cooker are accessible via the factory
  PipelineTestFixture Fixture;
  WriteFile(Fixture.SourceDir.Path / "bulk.testasset", "BULK_TEST");

  // Create a factory that also reads bulk data
  class BulkReadingFactory : public TAssetFactory<TestRuntimeObject>
  {
    public:
      TypeId GetCookedPayloadType() const override
      {
        return kTestCookedType;
      }

    protected:
      std::expected<TestRuntimeObject, std::string> DoLoad(const AssetLoadContext& Context) override
      {
        TestRuntimeObject Obj;
        Obj.OriginalContent = std::string(Context.Cooked.Bytes.begin(), Context.Cooked.Bytes.end());

        // Try loading bulk chunk 0
        auto BulkResult = Context.LoadBulk(0);
        if (BulkResult.has_value())
        {
          std::string BulkStr(BulkResult->begin(), BulkResult->end());
          Obj.OriginalContent += "|" + BulkStr;
        }

        Obj.ProcessedLength = static_cast<uint32_t>(Obj.OriginalContent.size());
        return Obj;
      }
  };

  AssetManagerConfig Config;
  Config.bEnableSourceAssets = true;
  SourceMountConfig SourceMount;
  SourceMount.RootPath = Fixture.SourceDir.Path.string();
  Config.SourceRoots.push_back(SourceMount);
  Config.PipelineConfig.OutputPackPath = (Fixture.OutputDir.Path / "test_runtime.snpak").string();
  Config.PipelineConfig.bDeterministicAssetIds = true;

  AssetManager Manager(Config);

  Manager.RegisterSerializer(std::make_unique<MockBytesPayloadSerializer>(kTestCookedType, "MockCookedPayload"));
  Manager.RegisterImporter(std::make_unique<MockImporter>());
  Manager.RegisterCooker(std::make_unique<MockCooker>());
  Manager.RegisterFactory<TestRuntimeObject>(std::make_unique<BulkReadingFactory>());

  auto Result = Manager.Load<TestRuntimeObject>("bulk.testasset");
  REQUIRE(Result.has_value());
  // Should contain cooked content + bulk data
  REQUIRE(Result->get()->OriginalContent.find("COOKED:BULK_TEST") != std::string::npos);
  REQUIRE(Result->get()->OriginalContent.find("BULK_DATA_FOR_bulk.testasset") != std::string::npos);
}

TEST_CASE("AssetManager pipeline handles multiple source files", "[source][pipeline]")
{
  PipelineTestFixture Fixture;
  WriteFile(Fixture.SourceDir.Path / "asset1.testasset", "FIRST");
  WriteFile(Fixture.SourceDir.Path / "asset2.testasset", "SECOND");

  auto Manager = Fixture.CreateManager();

  auto Result1 = Manager->Load<TestRuntimeObject>("asset1.testasset");
  auto Result2 = Manager->Load<TestRuntimeObject>("asset2.testasset");

  REQUIRE(Result1.has_value());
  REQUIRE(Result2.has_value());
  REQUIRE(Result1->get()->OriginalContent == "COOKED:FIRST");
  REQUIRE(Result2->get()->OriginalContent == "COOKED:SECOND");
  REQUIRE(Fixture.ImporterPtr->ImportCount == 2);
  REQUIRE(Fixture.CookerPtr->CookCount == 2);
  REQUIRE(Manager->GetDirtyAssetCount() == 2);
}
