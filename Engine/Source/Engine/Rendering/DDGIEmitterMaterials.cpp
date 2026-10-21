#include "Engine/Rendering/DDGIEmitterMaterials.h"

#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Types/StringId.h"

namespace GameEngine::Engine::Renderer
{

namespace
{

constexpr StringId kEmissiveColorName = HashStringId("emissive");
constexpr StringId kEmissionLuminanceName = HashStringId("emissionLuminance");

// Sentinel for Material::GetGpuSceneMaterialIndex on a material with no SSBO
// row (Material::kInvalidSSBOIndex, which is private).
constexpr uint32_t kUnassignedMaterialSlot = 0xFFFFFFFFu;

}  // namespace

void DDGIEmitterMaterials::Refresh(const MaterialSystem& materials)
{
    const MaterialRegistryStamp stamp = materials.GetRegistryStamp();
    if (m_HaveSweptStamp && stamp == m_SweptStamp)
        return;
    m_SweptStamp = stamp;
    m_HaveSweptStamp = true;

    for (Slot& slot : m_BySlot)
        slot.Emits = false;
    materials.Registry().ForEach(
        [this](const GUID&, const Material& material)
        {
            const uint32_t slotIndex = material.GetGpuSceneMaterialIndex();
            if (slotIndex == kUnassignedMaterialSlot)
                return;
            float tint[3] = {0.0f, 0.0f, 0.0f};
            material.GetVector(kEmissiveColorName, tint, 3);
            const float nits = material.GetFloat(kEmissionLuminanceName, 0.0f);
            const DDGIEmissive emissive = ComputeDDGIEmissive(tint[0], tint[1], tint[2], nits);
            if (!(emissive.Color[0] > 0.0f || emissive.Color[1] > 0.0f || emissive.Color[2] > 0.0f))
                return;
            if (slotIndex >= m_BySlot.size())
                m_BySlot.resize(slotIndex + 1);
            m_BySlot[slotIndex] = Slot{Emitter{emissive, material.IsDoubleSided()}, true};
        });
}

const DDGIEmitterMaterials::Emitter* DDGIEmitterMaterials::Find(uint32_t gpuSceneMaterialIndex) const
{
    if (gpuSceneMaterialIndex >= m_BySlot.size() || !m_BySlot[gpuSceneMaterialIndex].Emits)
        return nullptr;
    return &m_BySlot[gpuSceneMaterialIndex].Value;
}

}  // namespace GameEngine::Engine::Renderer
