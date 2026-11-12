// The files a parser names for an asset decode are read on the AssetIOService's reader thread
// before the decode job starts, so the decode does not block a JobSystem worker on them
// (AssetDecodeInputFiles.h). A texture's cooked artifact and a glTF's external buffers and
// images are read with the asset.
//
// The pipeline tests hold every pool worker until the decode job is queued, which is after its
// reads, then delete the file the decode needs. A decode that reads the disk itself finds the
// file gone: the texture recooks and writes its artifact again, and the glTF is refused. A
// decode that takes what the reader read loads as if the file were still there.

#include <gtest/gtest.h>

#include "AssetCore/SharedFileRead.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/AssetTasks.h"
#include "Assets/AuthoredLodImport.h"
#include "Assets/ModelAsset.h"
#include "Assets/RuntimeAssetMetadata.h"
#include "Assets/TextureAsset.h"
#include "Assets/TextureCook.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Mathematics/Vector3.h"
#include "TestTempDir.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace GameEngine;
using GameEngine::Mathematics::Vector3;

namespace
{

constexpr auto kTimeout = std::chrono::seconds(20);
constexpr size_t kPoolWorkers = 2;

void WriteBytes(const std::filesystem::path& path, const std::vector<uint8>& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

ByteBuffer ReadBytes(const std::filesystem::path& path)
{
    ByteBuffer bytes;
    EXPECT_TRUE(ReadFileBytesShared(path, bytes)) << path;
    return bytes;
}

// An uncompressed 32-bit top-left TGA. The cook resolves to Uncompressed with no GPU, and still
// runs, because mips are on by default.
std::vector<uint8> TestTga(uint32 size)
{
    std::vector<uint8> tga(18, 0);
    tga[2] = 2;
    tga[12] = static_cast<uint8>(size & 0xFF);
    tga[13] = static_cast<uint8>(size >> 8);
    tga[14] = static_cast<uint8>(size & 0xFF);
    tga[15] = static_cast<uint8>(size >> 8);
    tga[16] = 32;
    tga[17] = 0x20;
    for (uint32 y = 0; y < size; ++y)
        for (uint32 x = 0; x < size; ++x)
            tga.insert(tga.end(), {static_cast<uint8>(x), static_cast<uint8>(y), static_cast<uint8>(x ^ y), 255});
    return tga;
}

// One triangle's positions, the external buffer TriangleGltf names.
std::vector<uint8> TriangleBuffer()
{
    const std::array<Vector3, 3> positions = {Vector3(0.0f, 0.0f, 0.0f), Vector3(1.0f, 0.0f, 0.0f),
                                              Vector3(0.0f, 1.0f, 0.0f)};
    std::vector<uint8> buffer(sizeof(positions));
    std::memcpy(buffer.data(), positions.data(), buffer.size());
    return buffer;
}

// A .gltf whose one mesh is the triangle in the external buffer `bin`; `images` adds top-level
// members and `primitive` members of its primitive.
std::string TriangleGltf(const std::string& bin, const std::string& images = {}, const std::string& primitive = {})
{
    return R"({
  "asset": {"version": "2.0"},
  "buffers": [{"uri": ")" + bin + R"(", "byteLength": 36}],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}],
  "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
                 "min": [0, 0, 0], "max": [1, 1, 0]}],)" + images + R"(
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0})" + primitive + R"(}]}],
  "nodes": [{"mesh": 0}],
  "scenes": [{"nodes": [0]}],
  "scene": 0
})";
}

class AssetDecodeInputReadTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Root = TestUtils::MakeUniqueTempDirectory("asset_decode_input_read");
        m_Project = m_Root / "Project";
        std::filesystem::create_directories(m_Project);
    }

    void TearDown() override
    {
        TextureAsset::SetGpuTranscodeTarget(m_PreviousTranscodeTarget);
        ReleaseWorkers();
        if (m_Manager)
            m_Manager->Shutdown();
        m_Manager.reset();
        if (m_JobSystem)
            m_JobSystem->Shutdown();
        m_JobSystem.reset();
        std::error_code ec;
        std::filesystem::remove_all(m_Root, ec);
    }

    // Files written first, so the startup scan registers them.
    void StartManager()
    {
        m_JobSystem = std::make_unique<JobSystem::WorkStealingThreadPool>(kPoolWorkers);
        m_Manager = std::make_unique<AssetManager>();
        // The db file and cache root give the registry a derived-cache root to cook into.
        ASSERT_TRUE(m_Manager->Initialize(m_Project, m_JobSystem.get(), m_Project / "AssetDatabase.assetdb",
                                          m_Project / ".Cache" / "AssetDatabase"));
        m_Manager->WaitForStartupScan();
    }

    AssetMetadata Metadata(const std::filesystem::path& path) const
    {
        AssetMetadata metadata;
        EXPECT_TRUE(m_Manager->GetRegistry().TryGetAssetMetadata(path, metadata)) << path;
        return metadata;
    }

    std::vector<std::filesystem::path> CookedArtifacts() const
    {
        std::vector<std::filesystem::path> artifacts;
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it(m_Project, ec), end; !ec && it != end; it.increment(ec))
        {
            if (it->is_regular_file(ec) && it->path().extension() == ".ktx2")
                artifacts.push_back(it->path());
        }
        return artifacts;
    }

    // Occupies every pool worker, so a load's decode job queues after its reads.
    void HoldWorkers()
    {
        for (size_t worker = 0; worker < kPoolWorkers; ++worker)
            m_JobSystem->Submit([this] {
                m_Held.fetch_add(1);
                m_Release.wait();
            });
        const auto deadline = std::chrono::steady_clock::now() + kTimeout;
        while (m_Held.load() < kPoolWorkers && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ASSERT_EQ(m_Held.load(), kPoolWorkers);
    }

    void ReleaseWorkers()
    {
        if (!m_Released.exchange(true))
            m_ReleasePromise.set_value();
    }

    // True once a decode job is queued on the held pool: the reader has read everything.
    bool AwaitQueuedDecode() const
    {
        const auto deadline = std::chrono::steady_clock::now() + kTimeout;
        while (m_JobSystem->GetBackgroundQueuedForTests() == 0 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return m_JobSystem->GetBackgroundQueuedForTests() > 0;
    }

    // A load in flight: the handle keeps it requested, the future resolves with the asset (null
    // when the load failed).
    struct PendingLoad
    {
        AssetLoadHandle Handle;
        std::future<SharedPtr<Asset>> Loaded;
    };

    // Starts the load and returns, so the test thread can act while the decode is queued.
    PendingLoad StartLoad(const GUID& guid)
    {
        auto result = std::make_shared<std::promise<SharedPtr<Asset>>>();
        PendingLoad load{{}, result->get_future()};
        load.Handle = m_Manager->LoadAsset(guid, [result](Result<SharedPtr<Asset>, AssetError> loadResult) {
            result->set_value(loadResult.IsOk() ? loadResult.Value() : SharedPtr<Asset>{});
        });
        return load;
    }

    SharedPtr<Asset> Load(const GUID& guid)
    {
        PendingLoad load = StartLoad(guid);
        return load.Loaded.wait_for(kTimeout) == std::future_status::ready ? load.Loaded.get() : SharedPtr<Asset>{};
    }

    // A normal map on a BC device: its cook writes a BC5 artifact and the artifact's normal-variance companion.
    AssetMetadata StartNormalMap(const std::filesystem::path& source)
    {
        WriteBytes(source, TestTga(64));
        StartManager();
        EXPECT_TRUE(m_Manager->GetRegistry().SetMetaValue(source, kTextureUsageMetaKey, "normal"));
        TextureAsset::SetGpuTranscodeTarget(TextureGpuTranscodeTarget::BC7);
        return Metadata(source);
    }

    TextureGpuTranscodeTarget m_PreviousTranscodeTarget = TextureAsset::GetGpuTranscodeTarget();
    std::filesystem::path m_Root;
    std::filesystem::path m_Project;
    std::unique_ptr<JobSystem::WorkStealingThreadPool> m_JobSystem;
    std::unique_ptr<AssetManager> m_Manager;
    std::atomic<size_t> m_Held{0};
    std::atomic<bool> m_Released{false};
    std::promise<void> m_ReleasePromise;
    std::shared_future<void> m_Release = m_ReleasePromise.get_future().share();
};

void ExpectSameTexture(const SharedPtr<Asset>& actual, const SharedPtr<Asset>& expected)
{
    const auto* actualTexture = dynamic_cast<const TextureAsset*>(actual.get());
    const auto* expectedTexture = dynamic_cast<const TextureAsset*>(expected.get());
    ASSERT_NE(actualTexture, nullptr);
    ASSERT_NE(expectedTexture, nullptr);
    EXPECT_EQ(actualTexture->GetWidth(), expectedTexture->GetWidth());
    EXPECT_EQ(actualTexture->GetFormat(), expectedTexture->GetFormat());
    EXPECT_EQ(actualTexture->GetMipChain().size(), expectedTexture->GetMipChain().size());
    ASSERT_EQ(actualTexture->GetDataSize(), expectedTexture->GetDataSize());
    EXPECT_EQ(std::memcmp(actualTexture->GetPixelData(), expectedTexture->GetPixelData(),
                          static_cast<size_t>(actualTexture->GetDataSize())),
              0);
}

void ExpectSameModel(const ModelAsset& actual, const ModelAsset& expected)
{
    ASSERT_EQ(actual.GetMeshes().size(), expected.GetMeshes().size());
    for (size_t meshIndex = 0; meshIndex < actual.GetMeshes().size(); ++meshIndex)
    {
        const Mesh& actualMesh = actual.GetMeshes()[meshIndex];
        const Mesh& expectedMesh = expected.GetMeshes()[meshIndex];
        EXPECT_EQ(actualMesh.Indices, expectedMesh.Indices);
        ASSERT_EQ(actualMesh.Vertices.size(), expectedMesh.Vertices.size());
        EXPECT_EQ(std::memcmp(actualMesh.Vertices.data(), expectedMesh.Vertices.data(),
                              actualMesh.Vertices.size() * sizeof(Vertex)),
                  0);
    }
    ASSERT_EQ(actual.GetEmbeddedImages().size(), expected.GetEmbeddedImages().size());
    for (size_t imageIndex = 0; imageIndex < actual.GetEmbeddedImages().size(); ++imageIndex)
    {
        EXPECT_TRUE(std::ranges::equal(actual.GetEmbeddedImages()[imageIndex].Data,
                                       expected.GetEmbeddedImages()[imageIndex].Data));
        EXPECT_EQ(actual.GetEmbeddedImages()[imageIndex].MimeType, expected.GetEmbeddedImages()[imageIndex].MimeType);
    }
}

TEST_F(AssetDecodeInputReadTest, ColdTextureLoadCooksTheTextureADirectDecodeAdopts)
{
    const auto source = m_Project / "cold.tga";
    WriteBytes(source, TestTga(64));
    StartManager();
    const AssetMetadata metadata = Metadata(source);
    ASSERT_TRUE(CookedArtifacts().empty());

    const SharedPtr<Asset> loaded = Load(metadata.Guid);
    ASSERT_EQ(CookedArtifacts().size(), 1u) << "the pipeline load did not cook";
    const SharedPtr<Asset> direct = DecodeAssetPayload(*m_Manager, metadata.Guid, metadata, ReadBytes(source));
    ExpectSameTexture(loaded, direct);
    ASSERT_NE(dynamic_cast<const TextureAsset*>(loaded.get()), nullptr);
    EXPECT_GT(dynamic_cast<const TextureAsset*>(loaded.get())->GetMipChain().size(), 1u) << "the load raw-decoded";
}

TEST_F(AssetDecodeInputReadTest, TextureDecodeAdoptsTheArtifactReadBeforeIt)
{
    const auto source = m_Project / "warm.tga";
    WriteBytes(source, TestTga(64));
    StartManager();
    const AssetMetadata metadata = Metadata(source);
    // Outside the pipeline the decode reads the disk: the first cooks, the second adopts.
    ASSERT_NE(DecodeAssetPayload(*m_Manager, metadata.Guid, metadata, ReadBytes(source)), nullptr);
    const std::vector<std::filesystem::path> artifacts = CookedArtifacts();
    ASSERT_EQ(artifacts.size(), 1u);
    const SharedPtr<Asset> direct = DecodeAssetPayload(*m_Manager, metadata.Guid, metadata, ReadBytes(source));

    HoldWorkers();
    PendingLoad load = StartLoad(metadata.Guid);
    ASSERT_TRUE(AwaitQueuedDecode());
    std::filesystem::remove(artifacts.front());
    ReleaseWorkers();

    ASSERT_EQ(load.Loaded.wait_for(kTimeout), std::future_status::ready);
    const SharedPtr<Asset> loaded = load.Loaded.get();
    EXPECT_TRUE(CookedArtifacts().empty()) << "the decode read the deleted artifact from disk and cooked again";
    ExpectSameTexture(loaded, direct);
}

// A texture whose read sees the GPU transcode target RGBA32 and whose decode sees BC7: BC7 is viable
// only for the decode, but the decode takes the cook plan its reader resolved, so it adopts the
// uncompressed artifact the reader read and cooks nothing.
TEST_F(AssetDecodeInputReadTest, TextureDecodeTakesTheCookPlanItsReaderResolved)
{
    if (!IsTextureCookEncoderAvailable())
        GTEST_SKIP() << "No BC encoder is compiled in, so no plan here names a BC7 artifact";
    const auto source = m_Project / "planned.tga";
    WriteBytes(source, TestTga(64));
    StartManager();
    ASSERT_TRUE(m_Manager->GetRegistry().SetMetaValue(source, kTextureCompressionMetaKey, "bc7"));
    const AssetMetadata metadata = Metadata(source);
    TextureAsset::SetGpuTranscodeTarget(TextureGpuTranscodeTarget::RGBA32);
    // Outside the pipeline the decode resolves its own plan: under RGBA32 it cooks the uncompressed artifact.
    const SharedPtr<Asset> direct = DecodeAssetPayload(*m_Manager, metadata.Guid, metadata, ReadBytes(source));
    ASSERT_NE(direct, nullptr);
    const std::vector<std::filesystem::path> artifacts = CookedArtifacts();
    ASSERT_EQ(artifacts.size(), 1u);

    HoldWorkers();
    PendingLoad load = StartLoad(metadata.Guid);
    ASSERT_TRUE(AwaitQueuedDecode());
    TextureAsset::SetGpuTranscodeTarget(TextureGpuTranscodeTarget::BC7);
    ReleaseWorkers();

    ASSERT_EQ(load.Loaded.wait_for(kTimeout), std::future_status::ready);
    const SharedPtr<Asset> loaded = load.Loaded.get();
    EXPECT_EQ(CookedArtifacts(), artifacts) << "the decode resolved a plan of its own and cooked its artifact";
    ExpectSameTexture(loaded, direct);
}

void ExpectNormalMapWithVarianceCompanion(const SharedPtr<Asset>& asset)
{
    const auto* texture = dynamic_cast<const TextureAsset*>(asset.get());
    ASSERT_NE(texture, nullptr);
    EXPECT_EQ(texture->GetFormat(), TextureFormat::BC5);
    ASSERT_NE(texture->GetNormalVarianceCompanion(), nullptr) << "the normal map loaded without its variance companion";
    EXPECT_EQ(texture->GetNormalVarianceCompanion()->GetFormat(), TextureFormat::BC4);
}

TEST_F(AssetDecodeInputReadTest, NormalMapDecodeAdoptsTheArtifactAndCompanionReadBeforeIt)
{
    if (!IsTextureCookEncoderAvailable())
        GTEST_SKIP() << "No BC encoder is compiled in, so no normal map cooks to BC5";
    const auto source = m_Project / "normal.tga";
    const AssetMetadata metadata = StartNormalMap(source);
    const SharedPtr<Asset> direct = DecodeAssetPayload(*m_Manager, metadata.Guid, metadata, ReadBytes(source));
    const std::vector<std::filesystem::path> artifacts = CookedArtifacts();
    ASSERT_EQ(artifacts.size(), 2u) << "the cook did not write a BC5 artifact and its companion";

    HoldWorkers();
    PendingLoad load = StartLoad(metadata.Guid);
    ASSERT_TRUE(AwaitQueuedDecode());
    for (const std::filesystem::path& artifact : artifacts)
        std::filesystem::remove(artifact);
    ReleaseWorkers();

    ASSERT_EQ(load.Loaded.wait_for(kTimeout), std::future_status::ready);
    const SharedPtr<Asset> loaded = load.Loaded.get();
    EXPECT_TRUE(CookedArtifacts().empty()) << "the decode did not take the pair its reader read, and cooked again";
    ExpectSameTexture(loaded, direct);
    ExpectNormalMapWithVarianceCompanion(loaded);
}

// An artifact whose companion is absent is a miss, so the decode cooks the pair again.
TEST_F(AssetDecodeInputReadTest, NormalMapDecodeCooksWhenTheArtifactsCompanionIsAbsent)
{
    if (!IsTextureCookEncoderAvailable())
        GTEST_SKIP() << "No BC encoder is compiled in, so no normal map cooks to BC5";
    const auto source = m_Project / "normal.tga";
    const AssetMetadata metadata = StartNormalMap(source);
    ASSERT_NE(DecodeAssetPayload(*m_Manager, metadata.Guid, metadata, ReadBytes(source)), nullptr);
    std::filesystem::path companion;
    for (const std::filesystem::path& artifact : CookedArtifacts())
    {
        if (artifact.stem().extension() == ".nvar")
            companion = artifact;
    }
    ASSERT_FALSE(companion.empty()) << "the cook wrote no companion";
    std::filesystem::remove(companion);

    const SharedPtr<Asset> loaded = Load(metadata.Guid);
    EXPECT_TRUE(std::filesystem::exists(companion)) << "the decode adopted the artifact without its companion";
    ExpectNormalMapWithVarianceCompanion(loaded);
}

// A triangle in an external buffer, with an external image outside the project, which the
// AssetDatabase does not track, so the model copies its bytes.
TEST_F(AssetDecodeInputReadTest, GltfDecodeTakesExternalBufferAndImageReadBeforeIt)
{
    const std::vector<uint8> buffer = TriangleBuffer();
    const std::vector<uint8> image = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 1, 2, 3, 4};
    const auto bin = m_Project / "triangle.bin";
    const auto png = m_Root / "External" / "albedo.png";
    WriteBytes(bin, buffer);
    WriteBytes(png, image);
    const std::string gltf = TriangleGltf("triangle.bin", R"(
  "images": [{"uri": "../External/albedo.png"}],
  "textures": [{"source": 0}],
  "materials": [{"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}}],)",
                                          R"(, "material": 0)");
    const auto model = m_Project / "triangle.gltf";
    WriteBytes(model, std::vector<uint8>(gltf.begin(), gltf.end()));
    StartManager();
    const AssetMetadata metadata = Metadata(model);
    // Outside the pipeline the decode reads the external files from the disk.
    const auto direct = std::dynamic_pointer_cast<ModelAsset>(
        DecodeAssetPayload(*m_Manager, metadata.Guid, metadata, ReadBytes(model)));
    ASSERT_NE(direct, nullptr);

    HoldWorkers();
    PendingLoad load = StartLoad(metadata.Guid);
    ASSERT_TRUE(AwaitQueuedDecode());
    std::filesystem::remove(bin);
    std::filesystem::remove(png);
    ReleaseWorkers();

    ASSERT_EQ(load.Loaded.wait_for(kTimeout), std::future_status::ready);
    const auto loaded = std::dynamic_pointer_cast<ModelAsset>(load.Loaded.get());
    ASSERT_NE(loaded, nullptr) << "the decode read the deleted external files from disk";
    ASSERT_EQ(loaded->GetMeshes().size(), 1u);
    EXPECT_EQ(loaded->GetMeshes()[0].Vertices.size(), 3u);
    ASSERT_EQ(loaded->GetEmbeddedImages().size(), 1u);
    const ByteBuffer& loadedImage = loaded->GetEmbeddedImages()[0].Data;
    EXPECT_EQ(std::vector<uint8>(loadedImage.begin(), loadedImage.end()), image);
    ExpectSameModel(*loaded, *direct);
}

// A model's authored LOD slot loads synchronously inside the model's decode (PostLoad). It
// reads its own external buffer from the disk, not from the files read for the enclosing decode.
TEST_F(AssetDecodeInputReadTest, LodSlotModelLoadedInsideADecodeReadsItsOwnExternalBuffer)
{
    WriteBytes(m_Project / "base.bin", TriangleBuffer());
    WriteBytes(m_Project / "lod1.bin", TriangleBuffer());
    const std::string baseGltf = TriangleGltf("base.bin");
    const std::string lodGltf = TriangleGltf("lod1.bin");
    const auto base = m_Project / "base.gltf";
    const auto lod = m_Project / "lod1.gltf";
    WriteBytes(base, std::vector<uint8>(baseGltf.begin(), baseGltf.end()));
    WriteBytes(lod, std::vector<uint8>(lodGltf.begin(), lodGltf.end()));
    StartManager();
    const AssetMetadata baseMetadata = Metadata(base);
    const AssetMetadata lodMetadata = Metadata(lod);
    ASSERT_TRUE(SaveLodSlotRef(m_Manager->GetRegistry(), baseMetadata.Path, 1, lodMetadata.Guid,
                               lodMetadata.Path.generic_string()));

    const auto loaded = std::dynamic_pointer_cast<ModelAsset>(Load(baseMetadata.Guid));
    ASSERT_NE(loaded, nullptr);
    ASSERT_EQ(loaded->GetMeshes().size(), 1u);
    EXPECT_TRUE(loaded->GetMeshes()[0].HasAuthoredLODs()) << "the LOD slot model did not load";
    EXPECT_EQ(loaded->GetMeshes()[0].ExtraLODs.size(), 1u);
}

} // namespace
