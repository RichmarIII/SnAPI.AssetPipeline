// OpenGL Texture Example - Production Features Demo
// Demonstrates the full SnAPI.AssetPipeline workflow including:
// 1. Creating cooked texture data programmatically
// 2. Writing to .snpak with variants, mip levels, compression
// 3. Loading via AssetManager with custom factory
// 4. ** NEW ** Async loading with priority queues
// 5. ** NEW ** Ref-counted asset cache (AssetHandle<T>)
// 6. ** NEW ** Pack overlay/mounting priority system
// 7. ** NEW ** Hot-reload for development
// 8. ** NEW ** Memory-mapped streaming
// 9. Displays textures using OpenGL

#include <AssetPackWriter.h>
#include <AssetPackReader.h>
#include <AssetManager.h>
#include <MemoryMappedFile.h>

// Use the existing texture plugin headers - NO duplication!
#include "../../plugins/Texture/TexturePluginIds.h"
#include "../../plugins/Texture/TexturePluginPayloads.h"
#include "../../plugins/Texture/TexturePayloadSerializers.h"

#include <GLFW/glfw3.h>

#include <iostream>
#include <cstring>
#include <cmath>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>

using namespace SnAPI::AssetPipeline;

// ============================================================================
// PART 1: Game runtime texture class
// ============================================================================

// This is what your game uses - completely independent of the pipeline
struct GameTexture
{
    GLuint GlHandle = 0;
    uint32_t Width = 0;
    uint32_t Height = 0;
    uint32_t MipCount = 0;
    std::string Name;
    std::string Variant;

    // Raw pixel data for deferred GL upload (populated by async load, consumed by Finalize)
    std::vector<std::vector<uint8_t>> MipData;

    ~GameTexture()
    {
        if (GlHandle != 0)
        {
            glDeleteTextures(1, &GlHandle);
        }
    }

    // Finalize: Create GL texture from raw data (MUST be called from main/GL thread)
    // Returns true if GL texture was created, false if already finalized or no data
    bool Finalize()
    {
        if (GlHandle != 0 || MipData.empty())
        {
            return false;  // Already finalized or no data to upload
        }

        glGenTextures(1, &GlHandle);
        glBindTexture(GL_TEXTURE_2D, GlHandle);

        uint32_t MipWidth = Width;
        uint32_t MipHeight = Height;

        for (uint32_t Mip = 0; Mip < MipCount && Mip < MipData.size(); ++Mip)
        {
            glTexImage2D(GL_TEXTURE_2D, Mip, GL_RGBA8, MipWidth, MipHeight,
                         0, GL_RGBA, GL_UNSIGNED_BYTE, MipData[Mip].data());
            MipWidth = std::max(1u, MipWidth / 2);
            MipHeight = std::max(1u, MipHeight / 2);
        }

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        MipCount > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, MipCount - 1);

        glBindTexture(GL_TEXTURE_2D, 0);

        // Free the CPU-side data now that it's uploaded to GPU
        MipData.clear();
        MipData.shrink_to_fit();

        return true;
    }

    bool IsFinalized() const { return GlHandle != 0; }

    // Move only
    GameTexture() = default;
    GameTexture(GameTexture&& Other) noexcept
        : GlHandle(Other.GlHandle), Width(Other.Width), Height(Other.Height),
          MipCount(Other.MipCount), Name(std::move(Other.Name)), Variant(std::move(Other.Variant)),
          MipData(std::move(Other.MipData))
    {
        Other.GlHandle = 0;
    }
    GameTexture& operator=(GameTexture&& Other) noexcept
    {
        if (this != &Other)
        {
            if (GlHandle != 0) glDeleteTextures(1, &GlHandle);
            GlHandle = Other.GlHandle;
            Width = Other.Width;
            Height = Other.Height;
            MipCount = Other.MipCount;
            Name = std::move(Other.Name);
            Variant = std::move(Other.Variant);
            MipData = std::move(Other.MipData);
            Other.GlHandle = 0;
        }
        return *this;
    }
    GameTexture(const GameTexture&) = delete;
    GameTexture& operator=(const GameTexture&) = delete;
};

// ============================================================================
// PART 2: Asset Factory - converts cooked data to GameTexture
// ============================================================================

class GameTextureFactory : public TAssetFactory<GameTexture>
{
public:
    TypeId GetCookedPayloadType() const override
    {
        return TexturePlugin::Payload_TextureCookedInfo;
    }

protected:
    std::expected<GameTexture, std::string> DoLoad(const AssetLoadContext& Ctx) override
    {
        // Deserialize the cooked metadata using the PayloadRegistry
        auto InfoResult = Ctx.DeserializeCooked<TexturePlugin::TextureCookedInfo>();
        if (!InfoResult.has_value())
        {
            return std::unexpected(InfoResult.error());
        }
        const auto& Info = *InfoResult;

        GameTexture Tex;
        Tex.Width = Info.Width;
        Tex.Height = Info.Height;
        Tex.MipCount = Info.MipCount;
        Tex.Name = Ctx.Info.Name;
        Tex.Variant = Ctx.Info.VariantKey;

        // Load each mip level from bulk chunks into CPU memory
        // NOTE: No OpenGL calls here - this can run on any thread!
        // Call Tex.Finalize() from the main/GL thread to create the GPU texture.
        Tex.MipData.reserve(Info.MipCount);

        for (uint32_t Mip = 0; Mip < Info.MipCount; ++Mip)
        {
            auto Pixels = Ctx.LoadBulk(Mip);
            if (!Pixels.has_value())
            {
                return std::unexpected("Failed to load mip " + std::to_string(Mip) + ": " + Pixels.error());
            }
            Tex.MipData.push_back(std::move(*Pixels));
        }

        return Tex;
    }
};

// ============================================================================
// PART 3: Helper to generate test texture data
// ============================================================================

std::vector<uint8_t> GenerateCheckerboard(uint32_t Width, uint32_t Height,
                                           uint8_t R1, uint8_t G1, uint8_t B1,
                                           uint8_t R2, uint8_t G2, uint8_t B2,
                                           uint32_t CheckSize = 32)
{
    std::vector<uint8_t> Pixels(Width * Height * 4);
    for (uint32_t Y = 0; Y < Height; ++Y)
    {
        for (uint32_t X = 0; X < Width; ++X)
        {
            bool Check = ((X / CheckSize) + (Y / CheckSize)) % 2 == 0;
            size_t Idx = (Y * Width + X) * 4;
            Pixels[Idx + 0] = Check ? R1 : R2;
            Pixels[Idx + 1] = Check ? G1 : G2;
            Pixels[Idx + 2] = Check ? B1 : B2;
            Pixels[Idx + 3] = 255;
        }
    }
    return Pixels;
}

std::vector<uint8_t> GenerateGradient(uint32_t Width, uint32_t Height,
                                       uint8_t R, uint8_t G, uint8_t B)
{
    std::vector<uint8_t> Pixels(Width * Height * 4);
    for (uint32_t Y = 0; Y < Height; ++Y)
    {
        for (uint32_t X = 0; X < Width; ++X)
        {
            float U = static_cast<float>(X) / Width;
            float V = static_cast<float>(Y) / Height;
            size_t Idx = (Y * Width + X) * 4;
            Pixels[Idx + 0] = static_cast<uint8_t>(R * U);
            Pixels[Idx + 1] = static_cast<uint8_t>(G * V);
            Pixels[Idx + 2] = static_cast<uint8_t>(B * (1.0f - U));
            Pixels[Idx + 3] = 255;
        }
    }
    return Pixels;
}

// Generate mip chain by simple box filter
std::vector<std::vector<uint8_t>> GenerateMipChain(const std::vector<uint8_t>& Mip0,
                                                     uint32_t Width, uint32_t Height,
                                                     uint32_t MipCount)
{
    std::vector<std::vector<uint8_t>> Mips;
    Mips.push_back(Mip0);

    uint32_t MipW = Width;
    uint32_t MipH = Height;

    for (uint32_t M = 1; M < MipCount; ++M)
    {
        uint32_t PrevW = MipW;
        uint32_t PrevH = MipH;
        MipW = std::max(1u, MipW / 2);
        MipH = std::max(1u, MipH / 2);

        const auto& Prev = Mips.back();
        std::vector<uint8_t> Curr(MipW * MipH * 4);

        for (uint32_t Y = 0; Y < MipH; ++Y)
        {
            for (uint32_t X = 0; X < MipW; ++X)
            {
                uint32_t SrcX = X * 2;
                uint32_t SrcY = Y * 2;

                uint32_t R = 0, G = 0, B = 0, A = 0;
                for (uint32_t DY = 0; DY < 2 && SrcY + DY < PrevH; ++DY)
                {
                    for (uint32_t DX = 0; DX < 2 && SrcX + DX < PrevW; ++DX)
                    {
                        size_t SI = ((SrcY + DY) * PrevW + (SrcX + DX)) * 4;
                        R += Prev[SI + 0];
                        G += Prev[SI + 1];
                        B += Prev[SI + 2];
                        A += Prev[SI + 3];
                    }
                }

                size_t DI = (Y * MipW + X) * 4;
                Curr[DI + 0] = R / 4;
                Curr[DI + 1] = G / 4;
                Curr[DI + 2] = B / 4;
                Curr[DI + 3] = A / 4;
            }
        }

        Mips.push_back(std::move(Curr));
    }

    return Mips;
}

// ============================================================================
// PART 4: Create the .snpak files (base + patch overlay)
// ============================================================================

void CreateBasePack(const std::string& PackPath, const IPayloadSerializer& Serializer)
{
    std::cout << "Creating base pack: " << PackPath << "\n";

    AssetPackWriter Writer;
    Writer.SetCompression(EPackCompression::Zstd);

    // Texture 1: Red/Blue Checkerboard
    {
        const uint32_t Width = 256, Height = 256, MipCount = 4;
        auto Mip0 = GenerateCheckerboard(Width, Height, 255, 0, 0, 0, 0, 255, 32);
        auto MipChain = GenerateMipChain(Mip0, Width, Height, MipCount);

        TexturePlugin::TextureCookedInfo Info{Width, Height, MipCount, TexturePlugin::ETextureFormat::RGBA8};

        AssetPackEntry Entry;
        Entry.Id = Uuid::GenerateV5(TexturePlugin::AssetKind_Texture, "textures/checkerboard");
        Entry.AssetKind = TexturePlugin::AssetKind_Texture;
        Entry.Name = "textures/checkerboard";
        Entry.Cooked.PayloadType = TexturePlugin::Payload_TextureCookedInfo;
        Entry.Cooked.SchemaVersion = 1;
        Serializer.SerializeToBytes(&Info, Entry.Cooked.Bytes);

        for (uint32_t M = 0; M < MipCount; ++M)
        {
            BulkChunk Bulk;
            Bulk.Semantic = EBulkSemantic::Reserved_Level;
            Bulk.SubIndex = M;
            Bulk.bCompress = true;
            Bulk.Bytes = std::move(MipChain[M]);
            Entry.Bulk.push_back(std::move(Bulk));
        }
        Writer.AddAsset(std::move(Entry));
    }

    // Texture 2: Gradient
    {
        const uint32_t Width = 256, Height = 256, MipCount = 1;
        auto Pixels = GenerateGradient(Width, Height, 255, 255, 255);

        TexturePlugin::TextureCookedInfo Info{Width, Height, MipCount, TexturePlugin::ETextureFormat::RGBA8};

        AssetPackEntry Entry;
        Entry.Id = Uuid::GenerateV5(TexturePlugin::AssetKind_Texture, "textures/gradient");
        Entry.AssetKind = TexturePlugin::AssetKind_Texture;
        Entry.Name = "textures/gradient";
        Entry.Cooked.PayloadType = TexturePlugin::Payload_TextureCookedInfo;
        Entry.Cooked.SchemaVersion = 1;
        Serializer.SerializeToBytes(&Info, Entry.Cooked.Bytes);

        BulkChunk Bulk;
        Bulk.Semantic = EBulkSemantic::Reserved_Level;
        Bulk.SubIndex = 0;
        Bulk.bCompress = true;
        Bulk.Bytes = std::move(Pixels);
        Entry.Bulk.push_back(std::move(Bulk));
        Writer.AddAsset(std::move(Entry));
    }

    // Texture 3: Green texture (only in base)
    {
        const uint32_t Width = 128, Height = 128, MipCount = 1;
        auto Pixels = GenerateCheckerboard(Width, Height, 0, 255, 0, 0, 128, 0, 16);

        TexturePlugin::TextureCookedInfo Info{Width, Height, MipCount, TexturePlugin::ETextureFormat::RGBA8};

        AssetPackEntry Entry;
        Entry.Id = Uuid::GenerateV5(TexturePlugin::AssetKind_Texture, "textures/green");
        Entry.AssetKind = TexturePlugin::AssetKind_Texture;
        Entry.Name = "textures/green";
        Entry.Cooked.PayloadType = TexturePlugin::Payload_TextureCookedInfo;
        Entry.Cooked.SchemaVersion = 1;
        Serializer.SerializeToBytes(&Info, Entry.Cooked.Bytes);

        BulkChunk Bulk;
        Bulk.Semantic = EBulkSemantic::Reserved_Level;
        Bulk.SubIndex = 0;
        Bulk.bCompress = true;
        Bulk.Bytes = std::move(Pixels);
        Entry.Bulk.push_back(std::move(Bulk));
        Writer.AddAsset(std::move(Entry));
    }

    auto Result = Writer.Write(PackPath);
    if (!Result.has_value())
    {
        std::cerr << "Failed to write base pack: " << Result.error() << "\n";
    }
}

void CreatePatchPack(const std::string& PackPath, const IPayloadSerializer& Serializer)
{
    std::cout << "Creating patch pack (overlay): " << PackPath << "\n";

    AssetPackWriter Writer;
    Writer.SetCompression(EPackCompression::LZ4);  // Different compression for demo

    // Override the checkerboard with Yellow/Purple version
    {
        const uint32_t Width = 256, Height = 256, MipCount = 4;
        auto Mip0 = GenerateCheckerboard(Width, Height, 255, 255, 0, 128, 0, 128, 32);  // Yellow/Purple
        auto MipChain = GenerateMipChain(Mip0, Width, Height, MipCount);

        TexturePlugin::TextureCookedInfo Info{Width, Height, MipCount, TexturePlugin::ETextureFormat::RGBA8};

        AssetPackEntry Entry;
        Entry.Id = Uuid::GenerateV5(TexturePlugin::AssetKind_Texture, "textures/checkerboard");
        Entry.AssetKind = TexturePlugin::AssetKind_Texture;
        Entry.Name = "textures/checkerboard";
        Entry.Cooked.PayloadType = TexturePlugin::Payload_TextureCookedInfo;
        Entry.Cooked.SchemaVersion = 1;
        Serializer.SerializeToBytes(&Info, Entry.Cooked.Bytes);

        for (uint32_t M = 0; M < MipCount; ++M)
        {
            BulkChunk Bulk;
            Bulk.Semantic = EBulkSemantic::Reserved_Level;
            Bulk.SubIndex = M;
            Bulk.bCompress = true;
            Bulk.Bytes = std::move(MipChain[M]);
            Entry.Bulk.push_back(std::move(Bulk));
        }
        Writer.AddAsset(std::move(Entry));
    }

    // Add new texture only in patch
    {
      constexpr uint32_t Width = 128, Height = 128, MipCount = 1;
        auto Pixels = GenerateCheckerboard(Width, Height, 255, 128, 0, 0, 128, 255, 16);  // Orange/Cyan

        TexturePlugin::TextureCookedInfo Info{Width, Height, MipCount, TexturePlugin::ETextureFormat::RGBA8};

        AssetPackEntry Entry;
        Entry.Id = Uuid::GenerateV5(TexturePlugin::AssetKind_Texture, "textures/patch_only");
        Entry.AssetKind = TexturePlugin::AssetKind_Texture;
        Entry.Name = "textures/patch_only";
        Entry.Cooked.PayloadType = TexturePlugin::Payload_TextureCookedInfo;
        Entry.Cooked.SchemaVersion = 1;
        Serializer.SerializeToBytes(&Info, Entry.Cooked.Bytes);

        BulkChunk Bulk;
        Bulk.Semantic = EBulkSemantic::Reserved_Level;
        Bulk.SubIndex = 0;
        Bulk.bCompress = true;
        Bulk.Bytes = std::move(Pixels);
        Entry.Bulk.push_back(std::move(Bulk));
        Writer.AddAsset(std::move(Entry));
    }

    auto Result = Writer.Write(PackPath);
    if (!Result.has_value())
    {
        std::cerr << "Failed to write patch pack: " << Result.error() << "\n";
    }
}

// ============================================================================
// PART 5: Demonstrate memory-mapped streaming
// ============================================================================

void DemonstrateMemoryMappedStreaming(const std::string& PackPath)
{
    std::cout << "\n=== Memory-Mapped Streaming Demo ===\n";

    StreamingBulkReader StreamReader;
    auto OpenResult = StreamReader.Open(PackPath);
    if (!OpenResult.has_value())
    {
        std::cerr << "Failed to open pack for streaming: " << OpenResult.error() << "\n";
        return;
    }

    std::cout << "Pack file size: " << StreamReader.GetPackSize() << " bytes\n";

    // Read the header directly via memory mapping (zero-copy)
    auto HeaderSpan = StreamReader.ReadChunk(0, 180);  // SnPakHeaderV1 is 180 bytes
    if (HeaderSpan.has_value())
    {
        std::cout << "Header magic: ";
        for (int i = 0; i < 5; ++i) std::cout << static_cast<char>((*HeaderSpan)[i]);
        std::cout << "\n";
    }

    // Prefetch hint for upcoming reads
    StreamReader.PrefetchRange(0, 4096);
    std::cout << "Prefetched first 4KB for upcoming reads\n";

    StreamReader.Close();
}

// ============================================================================
// PART 6: Main - Full production features demo
// ============================================================================

int main()
{
    const std::string BasePackPath = "base_textures.snpak";
    const std::string PatchPackPath = "patch_textures.snpak";

    // Create the serializer using the plugin's factory function
    auto TextureSerializer = TexturePlugin::CreateTextureCookedInfoSerializer();

    // Step 1: Create the pack files
    std::cout << "\n========================================\n";
    std::cout << "PART 1: Creating Asset Packs\n";
    std::cout << "========================================\n";
    CreateBasePack(BasePackPath, *TextureSerializer);
    CreatePatchPack(PatchPackPath, *TextureSerializer);

    // Step 2: Demonstrate memory-mapped streaming
    std::cout << "\n========================================\n";
    std::cout << "PART 2: Memory-Mapped Streaming\n";
    std::cout << "========================================\n";
    DemonstrateMemoryMappedStreaming(BasePackPath);

    // Step 3: Initialize GLFW and OpenGL
    std::cout << "\n========================================\n";
    std::cout << "PART 3: Initializing OpenGL\n";
    std::cout << "========================================\n";

    if (!glfwInit())
    {
        std::cerr << "Failed to initialize GLFW\n";
        return 1;
    }

    GLFWwindow* Window = glfwCreateWindow(800, 600, "SnPak Production Features Demo", nullptr, nullptr);
    if (!Window)
    {
        std::cerr << "Failed to create window\n";
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(Window);

    // Step 4: Setup AssetManager with production features
    std::cout << "\n========================================\n";
    std::cout << "PART 4: AssetManager Setup\n";
    std::cout << "========================================\n";

    AssetManagerConfig Config;
    Config.CacheConfig.MaxMemoryBytes = 64 * 1024 * 1024;  // 64 MB cache
    Config.CacheConfig.MinAgeBeforeEviction = std::chrono::seconds(2LL);
    Config.AsyncLoaderThreads = 2;
    Config.bEnableHotReload = true;

    AssetManager Assets(Config);

    // Register the texture serializer
    Assets.GetRegistry().Register(std::move(TextureSerializer));

    // Register the factory
    Assets.RegisterFactory<GameTexture>(std::make_unique<GameTextureFactory>());

    // Mount packs with different priorities (demonstrating overlay system)
    std::cout << "\n--- Pack Overlay System ---\n";

    // Base pack at priority 0
    auto BaseResult = Assets.MountPack(BasePackPath, {.Priority = 0});
    if (!BaseResult.has_value())
    {
        std::cerr << "Failed to mount base pack: " << BaseResult.error() << "\n";
        glfwTerminate();
        return 1;
    }
    std::cout << "Mounted base pack at priority 0\n";

    // Patch pack at higher priority (will override base assets)
    auto PatchResult = Assets.MountPack(PatchPackPath, {.Priority = 100});
    if (!PatchResult.has_value())
    {
        std::cerr << "Failed to mount patch pack: " << PatchResult.error() << "\n";
    }
    else
    {
        std::cout << "Mounted patch pack at priority 100 (overrides base)\n";
    }

    std::cout << "Mounted packs: ";
    for (const auto& Path : Assets.GetMountedPacks())
    {
        std::cout << Path << " ";
    }
    std::cout << "\n";

    // Step 5: Enable hot-reload
    std::cout << "\n--- Hot-Reload System ---\n";
    Assets.SetHotReloadEnabled(true);
    Assets.SetHotReloadCallback([](const std::vector<AssetId>& ReloadedIds) {
        std::cout << "[HOT-RELOAD] " << ReloadedIds.size() << " assets invalidated!\n";
    });
    std::cout << "Hot-reload enabled (modify .snpak files to trigger)\n";

    // Step 6: Demonstrate async loading
    std::cout << "\n========================================\n";
    std::cout << "PART 5: Async Loading Demo\n";
    std::cout << "========================================\n";

    std::atomic<int> LoadedCount{0};
    std::vector<AsyncLoadHandle> AsyncHandles;

    std::cout << "Queueing async loads with different priorities...\n";

    // Queue loads with different priorities
    auto Handle1 = Assets.LoadAsync<GameTexture>("textures/gradient", ELoadPriority::Low,
        [&LoadedCount](AsyncLoadResult<GameTexture> Result) {
            if (Result.IsSuccess())
            {
                std::cout << "  [ASYNC] Loaded gradient (Low priority): "
                          << Result.Asset->Width << "x" << Result.Asset->Height << "\n";
            }
            else
            {
                std::cout << "  [ASYNC] Failed to load gradient: " << Result.Error << "\n";
            }
            ++LoadedCount;
        });
    AsyncHandles.push_back(Handle1);

    auto Handle2 = Assets.LoadAsync<GameTexture>("textures/green", ELoadPriority::Normal,
        [&LoadedCount](AsyncLoadResult<GameTexture> Result) {
            if (Result.IsSuccess())
            {
                std::cout << "  [ASYNC] Loaded green (Normal priority): "
                          << Result.Asset->Width << "x" << Result.Asset->Height << "\n";
            }
            ++LoadedCount;
        });
    AsyncHandles.push_back(Handle2);

    // Critical priority - should load first!
    auto Handle3 = Assets.LoadAsync<GameTexture>("textures/checkerboard", ELoadPriority::Critical,
        [&LoadedCount](AsyncLoadResult<GameTexture> Result) {
            if (Result.IsSuccess())
            {
                std::cout << "  [ASYNC] Loaded checkerboard (Critical priority): "
                          << Result.Asset->Width << "x" << Result.Asset->Height << "\n";
            }
            ++LoadedCount;
        });
    AsyncHandles.push_back(Handle3);

    std::cout << "Waiting for async loads to complete...\n";
    Assets.GetAsyncLoader().WaitAll();
    std::cout << "All " << LoadedCount.load() << " async loads completed!\n";

    // Step 7: Demonstrate cached loading with ref-counting
    std::cout << "\n========================================\n";
    std::cout << "PART 6: Cached Loading (Ref-Counted)\n";
    std::cout << "========================================\n";

    std::cout << "Loading 'textures/checkerboard' via cache (first load)...\n";
    auto CachedHandle1 = Assets.Get<GameTexture>("textures/checkerboard");
    if (CachedHandle1.has_value())
    {
        auto& Handle = *CachedHandle1;
        std::cout << "  Loaded! RefCount: " << Handle.UseCount()
                  << ", Size: " << Handle->Width << "x" << Handle->Height << "\n";
    }

    std::cout << "Getting same texture again (from cache)...\n";
    auto CachedHandle2 = Assets.Get<GameTexture>("textures/checkerboard");
    if (CachedHandle2.has_value())
    {
        std::cout << "  From cache! RefCount: " << CachedHandle2->UseCount() << "\n";
    }

    std::cout << "\nCache statistics:\n";
    std::cout << "  Cached entries: " << Assets.GetCache().GetCachedCount() << "\n";
    std::cout << "  Memory usage: " << Assets.GetCache().GetMemoryUsage() << " bytes\n";
    std::cout << "  Referenced entries: " << Assets.GetCache().GetReferencedCount() << "\n";

    // Load more textures to fill cache
    std::cout << "\nLoading more textures into cache...\n";
    auto TexGradient = Assets.Get<GameTexture>("textures/gradient");
    auto TexGreen = Assets.Get<GameTexture>("textures/green");
    auto TexPatchOnly = Assets.Get<GameTexture>("textures/patch_only");

    std::cout << "Cache after loading all:\n";
    std::cout << "  Cached entries: " << Assets.GetCache().GetCachedCount() << "\n";
    std::cout << "  Memory usage: " << Assets.GetCache().GetMemoryUsage() << " bytes\n";

    // Collect textures for rendering
    std::vector<AssetHandle<GameTexture>> TextureHandles;
    if (CachedHandle1.has_value()) TextureHandles.push_back(std::move(*CachedHandle1));
    if (TexGradient.has_value()) TextureHandles.push_back(std::move(*TexGradient));
    if (TexGreen.has_value()) TextureHandles.push_back(std::move(*TexGreen));
    if (TexPatchOnly.has_value()) TextureHandles.push_back(std::move(*TexPatchOnly));

    // Finalize all textures (create GL resources) - must be on main/GL thread!
    // This is the two-phase load pattern: async workers load raw data,
    // main thread creates GPU resources.
    std::cout << "\nFinalizing textures (creating GL resources)...\n";
    for (auto& Handle : TextureHandles)
    {
        if (Handle.IsValid() && !Handle->IsFinalized())
        {
            Handle->Finalize();
            std::cout << "  Finalized: " << Handle->Name << " (" << Handle->Width << "x" << Handle->Height << ")\n";
        }
    }

    // Let CachedHandle2 go out of scope (releases ref)

    size_t CurrentTexture = 0;

    // Step 8: Render loop
    std::cout << "\n========================================\n";
    std::cout << "PART 7: OpenGL Rendering\n";
    std::cout << "========================================\n";
    std::cout << "\n--- Controls ---\n";
    std::cout << "Left/Right Arrow: Switch texture\n";
    std::cout << "R: Check for hot-reload changes\n";
    std::cout << "C: Clear unreferenced cache entries\n";
    std::cout << "S: Show cache statistics\n";
    std::cout << "ESC: Exit\n";
    std::cout << "\nDisplaying " << TextureHandles.size() << " textures...\n";
    std::cout << "\nNOTE: The checkerboard is YELLOW/PURPLE because the patch pack\n";
    std::cout << "      overrides the RED/BLUE version from the base pack!\n";

    glEnable(GL_TEXTURE_2D);
    glClearColor(0.2f, 0.2f, 0.2f, 1.0f);

    auto LastHotReloadCheck = std::chrono::steady_clock::now();

    while (!glfwWindowShouldClose(Window))
    {
        glfwPollEvents();

        // Handle input
        if (glfwGetKey(Window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
        {
            glfwSetWindowShouldClose(Window, GLFW_TRUE);
        }

        static bool LeftPressed = false, RightPressed = false;
        static bool RPressed = false, CPressed = false, SPressed = false;

        if (glfwGetKey(Window, GLFW_KEY_LEFT) == GLFW_PRESS)
        {
            if (!LeftPressed && !TextureHandles.empty())
            {
                CurrentTexture = (CurrentTexture + TextureHandles.size() - 1) % TextureHandles.size();
                auto& Tex = TextureHandles[CurrentTexture];
                std::cout << "Showing: " << Tex->Name;
                if (!Tex->Variant.empty()) std::cout << " [" << Tex->Variant << "]";
                std::cout << " (" << Tex->Width << "x" << Tex->Height << ")\n";
            }
            LeftPressed = true;
        }
        else LeftPressed = false;

        if (glfwGetKey(Window, GLFW_KEY_RIGHT) == GLFW_PRESS)
        {
            if (!RightPressed && !TextureHandles.empty())
            {
                CurrentTexture = (CurrentTexture + 1) % TextureHandles.size();
                auto& Tex = TextureHandles[CurrentTexture];
                std::cout << "Showing: " << Tex->Name;
                if (!Tex->Variant.empty()) std::cout << " [" << Tex->Variant << "]";
                std::cout << " (" << Tex->Width << "x" << Tex->Height << ")\n";
            }
            RightPressed = true;
        }
        else RightPressed = false;

        // R: Manual hot-reload check
        if (glfwGetKey(Window, GLFW_KEY_R) == GLFW_PRESS)
        {
            if (!RPressed)
            {
                auto ReloadedPacks = Assets.CheckForChanges();
                if (!ReloadedPacks.empty())
                {
                    std::cout << "Reloaded " << ReloadedPacks.size() << " pack(s)\n";
                }
                else
                {
                    std::cout << "No changes detected\n";
                }
            }
            RPressed = true;
        }
        else RPressed = false;

        // C: Clear unreferenced cache
        if (glfwGetKey(Window, GLFW_KEY_C) == GLFW_PRESS)
        {
            if (!CPressed)
            {
                size_t Cleared = Assets.ClearUnreferencedCache();
                std::cout << "Cleared " << Cleared << " unreferenced cache entries\n";
            }
            CPressed = true;
        }
        else CPressed = false;

        // S: Show cache stats
        if (glfwGetKey(Window, GLFW_KEY_S) == GLFW_PRESS)
        {
            if (!SPressed)
            {
                std::cout << "Cache: " << Assets.GetCache().GetCachedCount() << " entries, "
                          << Assets.GetCache().GetMemoryUsage() << " bytes, "
                          << Assets.GetCache().GetReferencedCount() << " referenced\n";
            }
            SPressed = true;
        }
        else SPressed = false;

        // Auto hot-reload check every 2 seconds
        auto Now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(Now - LastHotReloadCheck).count() >= 2)
        {
            Assets.CheckForChanges();
            LastHotReloadCheck = Now;
        }

        // Render
        int Width, Height;
        glfwGetFramebufferSize(Window, &Width, &Height);
        glViewport(0, 0, Width, Height);
        glClear(GL_COLOR_BUFFER_BIT);

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(-1, 1, -1, 1, -1, 1);

        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        if (!TextureHandles.empty() && TextureHandles[CurrentTexture].IsValid())
        {
            glBindTexture(GL_TEXTURE_2D, TextureHandles[CurrentTexture]->GlHandle);
            glColor3f(1, 1, 1);

            glBegin(GL_QUADS);
            glTexCoord2f(0, 1); glVertex2f(-0.8f, -0.8f);
            glTexCoord2f(1, 1); glVertex2f( 0.8f, -0.8f);
            glTexCoord2f(1, 0); glVertex2f( 0.8f,  0.8f);
            glTexCoord2f(0, 0); glVertex2f(-0.8f,  0.8f);
            glEnd();
        }

        glfwSwapBuffers(Window);
    }

    // Step 9: Cleanup
    std::cout << "\n========================================\n";
    std::cout << "Cleanup\n";
    std::cout << "========================================\n";

    std::cout << "Releasing texture handles...\n";
    TextureHandles.clear();

    std::cout << "Cache after releasing handles:\n";
    std::cout << "  Cached entries: " << Assets.GetCache().GetCachedCount() << "\n";
    std::cout << "  Referenced entries: " << Assets.GetCache().GetReferencedCount() << "\n";

    size_t Evicted = Assets.ClearUnreferencedCache();
    std::cout << "  Cleared " << Evicted << " unreferenced entries\n";

    glfwDestroyWindow(Window);
    glfwTerminate();

    std::cout << "\nExample complete!\n";
    return 0;
}
