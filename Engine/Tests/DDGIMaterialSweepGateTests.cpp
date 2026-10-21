// DDGI derives two per-material tables from the material registry every tick:
// the map atlas's layer assignment and the emitter table its emissive NEE
// publishes. Both re-derive only when MaterialSystem::GetRegistryStamp moves.
// These tests pin both halves of that contract: a still stamp means no sweep,
// and every edit either table reads moves the stamp.

#include <gtest/gtest.h>

#include "Engine/Rendering/DDGIEmitterMaterials.h"
#include "Engine/Rendering/DDGIMaterialMapAtlas.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/TextureService.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "TestDeviceHelper.h"

#include <array>
#include <memory>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Rendering;

namespace
{

constexpr StringId kAlbedoMap = HashStringId("albedoMap");
constexpr StringId kEmissiveMap = HashStringId("emissiveMap");
constexpr StringId kEmissiveColor = HashStringId("emissive");
constexpr StringId kEmissionLuminance = HashStringId("emissionLuminance");

// A handle no device ever issued: the tables only compare and copy handles, so
// a distinct id stands for "a texture finished loading into this slot".
constexpr TextureHandle kStreamedTexture{0x1234u, 1u};

class DDGIMaterialSweepGate : public testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";
        m_Services = std::make_unique<RenderServices>();
        ASSERT_TRUE(m_Services->Initialize(m_Device.get()));
    }

    void TearDown() override
    {
        if (m_Services)
            m_Services->Shutdown();
        if (m_Device)
            m_Device->Shutdown();
    }

    MaterialSystem& Materials() { return m_Services->Materials(); }

    Material* Register(const GUID& guid)
    {
        const MaterialDocument doc = MaterialDocument::CreateDefaultPBR("SweepGate");
        return Materials().RegisterMaterialFromDocument(guid, doc);
    }

    Material* RegisterEmitter(const GUID& guid)
    {
        Material* mat = Register(guid);
        if (!mat)
            return nullptr;
        const std::array<float, 3> orange{1.0f, 0.5f, 0.0f};
        mat->SetVector(kEmissiveColor, orange.data(), 3);
        mat->SetFloat(kEmissionLuminance, 1000.0f);
        return mat;
    }

    // Whether the registry stamp moved since the last call (or since m_Stamp was set).
    bool StampMoved()
    {
        const MaterialRegistryStamp now = Materials().GetRegistryStamp();
        const bool changed = now != m_Stamp;
        m_Stamp = now;
        return changed;
    }

    std::unique_ptr<IDevice> m_Device;
    std::unique_ptr<RenderServices> m_Services;
    MaterialRegistryStamp m_Stamp;
};

} // namespace

// A material moved to another GPU-scene row behind the registry's back (the
// test factory moves the row without bumping anything) is invisible to a gated
// atlas: no sweep ran. The next stamped edit sweeps and finds the move.
TEST_F(DDGIMaterialSweepGate, TheMapAtlasDoesNotSweepAStillRegistry)
{
    Material* mat = Register(GUID::Generate());
    ASSERT_NE(mat, nullptr);
    mat->SetTexture(kAlbedoMap, kStreamedTexture);
    DDGIMaterialMapAtlas atlas(m_Device.get(), &Materials());

    atlas.Refresh();
    const uint32_t slot = mat->GetGpuSceneMaterialIndex();
    ASSERT_GE(atlas.GetRecordForMaterialSlot(slot).AlbedoLayer, 0.0f);

    const uint32_t movedSlot = slot + 7u;
    Material::TestFactory::SetGpuSceneMaterialIndex(*mat, movedSlot);
    atlas.Refresh();
    EXPECT_GE(atlas.GetRecordForMaterialSlot(slot).AlbedoLayer, 0.0f)
        << "the atlas swept a registry whose stamp had not moved";
    EXPECT_EQ(atlas.GetRecordForMaterialSlot(movedSlot).AlbedoLayer, DDGIMaterialMapAtlas::kNoLayer);

    const float scale[8] = {2.0f, 0.0f, 0.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f};
    mat->SetTextureTransform(TextureSlot::kAlbedo, scale);
    atlas.Refresh();
    EXPECT_GE(atlas.GetRecordForMaterialSlot(movedSlot).AlbedoLayer, 0.0f)
        << "a stamped edit must sweep again";
    EXPECT_EQ(atlas.GetRecordForMaterialSlot(movedSlot).ScaleX, 2.0f);

    Material::TestFactory::SetGpuSceneMaterialIndex(*mat, slot);
}

// The same contract for the emitter table, and a stamped emissive edit reaches it.
TEST_F(DDGIMaterialSweepGate, TheEmitterTableDoesNotSweepAStillRegistry)
{
    Material* mat = RegisterEmitter(GUID::Generate());
    ASSERT_NE(mat, nullptr);
    DDGIEmitterMaterials emitters;

    emitters.Refresh(Materials());
    const uint32_t slot = mat->GetGpuSceneMaterialIndex();
    const DDGIEmitterMaterials::Emitter* emitter = emitters.Find(slot);
    ASSERT_NE(emitter, nullptr);
    EXPECT_GT(emitter->Emissive.Color[0], emitter->Emissive.Color[1]);

    const uint32_t movedSlot = slot + 7u;
    Material::TestFactory::SetGpuSceneMaterialIndex(*mat, movedSlot);
    emitters.Refresh(Materials());
    EXPECT_NE(emitters.Find(slot), nullptr) << "the emitter table swept a registry whose stamp had not moved";
    EXPECT_EQ(emitters.Find(movedSlot), nullptr);

    mat->SetFloat(kEmissionLuminance, 0.0f);
    emitters.Refresh(Materials());
    EXPECT_EQ(emitters.Find(slot), nullptr) << "a stamped edit must sweep again";
    EXPECT_EQ(emitters.Find(movedSlot), nullptr) << "a material with no luminance emits nothing";

    Material::TestFactory::SetGpuSceneMaterialIndex(*mat, slot);
}

// Every edit the two tables read moves the stamp, and a replay that changes
// nothing (rebinding the same texture, a repeated wait on a load still
// pending, re-installing the same slot map) leaves it still.
TEST_F(DDGIMaterialSweepGate, EveryEditTheTablesReadMovesTheStampAndReplaysDoNot)
{
    const GUID guid = GUID::Generate();
    Material* mat = Register(guid);
    ASSERT_NE(mat, nullptr);
    m_Stamp = Materials().GetRegistryStamp();

    mat->SetFloat(kEmissionLuminance, 250.0f);
    EXPECT_TRUE(StampMoved()) << "a property edit";

    mat->SetTexture(kEmissiveMap, kStreamedTexture);
    EXPECT_TRUE(StampMoved()) << "a texture landing in a slot";
    mat->SetTexture(kEmissiveMap, kStreamedTexture);
    EXPECT_FALSE(StampMoved()) << "rebinding the same texture";

    m_Services->Textures().BindAwaitedTexture(mat, kEmissiveMap);
    EXPECT_TRUE(StampMoved()) << "a slot starting to wait for its texture";
    m_Services->Textures().BindAwaitedTexture(mat, kEmissiveMap);
    EXPECT_FALSE(StampMoved()) << "a replayed wait on a load still pending";
    mat->SetTexture(kEmissiveMap, TextureHandle{});
    EXPECT_TRUE(StampMoved()) << "a wait ending with no texture";

    mat->MarkTextureAwaited(kEmissiveMap);
    EXPECT_TRUE(StampMoved()) << "a wait on a slot that holds no texture";

    mat->SetTextureSlotMap({{"emissiveMap", 0}});
    EXPECT_TRUE(StampMoved()) << "a slot map that moves a name to another ordinal";
    mat->SetTextureSlotMap({{"emissiveMap", 0}});
    EXPECT_FALSE(StampMoved()) << "re-installing the same slot map";

    const GUID second = GUID::Generate();
    ASSERT_NE(Register(second), nullptr);
    EXPECT_TRUE(StampMoved()) << "a new material taking a GPU-scene row";
    Materials().Registry().Unregister(second);
    EXPECT_TRUE(StampMoved()) << "a material freeing its GPU-scene row";
}
