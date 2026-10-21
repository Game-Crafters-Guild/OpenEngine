#pragma once

#include "Engine/Rendering/IRenderFeature.h"
#include "Rendering/Core/RenderGraph/RGReadbackRing.h"

#include <cstdint>

namespace GameEngine::Engine::Renderer
{
class ImageBasedLightingFeature;

// The image-based light on an up-facing surface, in lux: what the scene's IBL irradiance cube
// holds, whichever environment source baked it — the sky (SkyEnvironmentSource), or a scene
// reflection probe the camera stands in (SceneReflectionProbeEnvironmentSource), which then lights
// the scene instead of the sky. Each source copies the centre texels of the cube's +Y face into a
// readback ring right after its diffuse convolve, so the value changes only when a bake reruns,
// which is only when the environment changes; it is never metered per frame. A consumer that
// arrives on a steady environment gets one copy of the cube as it rests (DeclareRestingCopy).
//
// Off until a consumer creates it (RenderServices::EnsureFeature): without the feature no source
// declares a copy and nothing is paid.
//
// What the value includes: the environment as baked, so a sky's SkyExposureTrim (the capture
// multiplies by exp2(exposureEV), sky_capture_cube.frag) is in it. What it does not include: the
// source's IblIntensity and ambient gradient tint, which ibl.glsl applies when it samples the cube,
// and an AmbientLight floor.
//
// Freshness: a reading lands once the frame that copied it has completed on the GPU, a frame or
// two after its bake. Until then TryGetUpIrradianceLux returns the previous reading, and its
// bake digest differs from ImageBasedLightingFeature::GetLastBakedDigest(); equal digests mean
// the reading is of the cube as it is now.
//
// Threads: everything here — the copy declaration, the submit fan-out and the read — runs on the
// render thread that records frames, which is the main thread in the editor and the player.
class IrradianceReadbackFeature final : public IRenderFeature
{
  public:
    // The copied block: kTexelSpan x kTexelSpan RGBA16F texels straddling the centre of the
    // +Y face, one row per kRowPitchBytes (the copy row alignment D3D12 and WebGPU require).
    static constexpr uint32_t kTexelSpan = 2;
    static constexpr uint32_t kRowPitchBytes = 256;
    static constexpr uint32_t kSlotBytes = kRowPitchBytes * kTexelSpan;
    // Array layer of the +Y face in the cube (+X, -X, +Y, -Y, +Z, -Z).
    static constexpr uint32_t kUpFaceLayer = 2;

    IrradianceReadbackFeature();
    ~IrradianceReadbackFeature() override;

    // True while no copy has been declared since the feature was created or the device was
    // rebuilt: the active source declares one on its next frame even when it does not bake.
    bool NeedsCopyWithoutBake() const { return m_NeedsCopyWithoutBake; }

    // Declares one copy of the cube as it rests, when NeedsCopyWithoutBake and the cube holds a
    // bake (its last baked digest is not 0). For a source that is not baking this frame.
    void DeclareRestingCopy(Rendering::IDevice& device, Rendering::RenderGraph::RGFrame& frame,
                            const ImageBasedLightingFeature& ibl);

    // Declares the copy of `irradianceCube`'s up-facing centre texels into the next ring slot.
    // `cubeSize` is the cube's face width in texels; `bakeDigest` is the sky input digest the
    // cube was baked for, carried with the slot.
    void DeclareCopy(Rendering::IDevice& device, Rendering::RenderGraph::RGFrame& frame,
                     Rendering::RenderGraph::RGTexture irradianceCube, uint32_t cubeSize,
                     uint64_t bakeDigest);

    void OnFrameSubmittedRG(Rendering::RenderGraph::RGFrame& frame,
                            const Rendering::IDevice::GpuSyncToken& token) override;
    void OnFrameStreamRetiredRG(Rendering::RenderGraph::RGFrame& frame) override;
    // The ring's slots are persistently mapped; a device rebuild frees them. Drop the ring so the
    // next copy re-creates it, and ask for a copy, since the rebuilt cube is baked again.
    void OnDeviceRebuilt(Rendering::IDevice* device) override;

    // The newest landed reading: the IBL irradiance on an up-facing surface, in lux, and the
    // digest of the bake it was read from. False until the first copy lands; between arrivals it
    // returns the last reading. Outputs are untouched on false.
    bool TryGetUpIrradianceLux(float& outLux, uint64_t* outBakeDigest = nullptr);

  private:
    struct SlotTag
    {
        uint64_t BakeDigest = 0;
    };

    Rendering::IDevice* m_Device = nullptr;
    Rendering::RenderGraph::RGReadbackRing<SlotTag> m_Ring;
    float m_LastLux = 0.0f;
    uint64_t m_LastBakeDigest = 0;
    bool m_HasReading = false;
    bool m_NeedsCopyWithoutBake = true;
};

} // namespace GameEngine::Engine::Renderer
