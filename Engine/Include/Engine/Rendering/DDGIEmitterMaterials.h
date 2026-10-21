#pragma once

#include "Engine/Rendering/DDGIEmissive.h"
#include "Engine/Rendering/MaterialRegistryStamp.h"

#include <cstdint>
#include <vector>

namespace GameEngine::Engine::Renderer
{

class MaterialSystem;

// The materials DDGI's emissive NEE publishes as sphere-proxy lights, indexed by
// GPU-scene material slot (GPUInstance.materialIndex): each one's scene-linear
// emissive and whether both faces emit. Derived from the material registry, and
// re-derived only when its stamp moves, because every tick reads it while the
// registry almost never changes; the sweep is two property lookups by name per
// material.
class DDGIEmitterMaterials
{
  public:
    struct Emitter
    {
        DDGIEmissive Emissive;
        bool DoubleSided = false;
    };

    // Called every tick; sweeps the registry only when its stamp moved since the
    // last sweep.
    void Refresh(const MaterialSystem& materials);

    // The emitting material in `gpuSceneMaterialIndex`, or null when that slot's
    // material emits nothing (or the slot holds no material).
    const Emitter* Find(uint32_t gpuSceneMaterialIndex) const;

  private:
    struct Slot
    {
        Emitter Value;
        bool Emits = false;
    };

    std::vector<Slot> m_BySlot;
    MaterialRegistryStamp m_SweptStamp;
    bool m_HaveSweptStamp = false;
};

}  // namespace GameEngine::Engine::Renderer
