// A user module that registers a terrain height function. The GE_PROBE_REFUSED build registers
// another name and fails its Register_v1, so the loader aborts the load and unmaps it.
#include "GameSDK/TerrainHeightFunction.h"
#include "NativeScripting/NativeScriptingABI.h"

namespace
{
using namespace GameEngine;

// Height = kSlope * x + kSeedStep * seed: a closed form the host test checks exactly.
constexpr float32 kSlope = 0.5f;
constexpr float32 kSeedStep = 2.0f;

class ProbeSlope final : public TerrainECS::ITerrainHeightFunction
{
public:
    explicit ProbeSlope(const TerrainECS::TerrainHeightFunctionContext& context)
        : m_Offset(kSeedStep * static_cast<float32>(context.Seed))
    {
    }

    void HeightRow(float32, std::span<const float32> x, float32, std::span<float32> outMeters) const override
    {
        for (std::size_t index = 0; index < x.size(); ++index)
            outMeters[index] = kSlope * x[index] + m_Offset;
    }

    TerrainECS::TerrainHeightRange HeightRange(float32 minX, float32, float32 maxX, float32, float32) const override
    {
        return {kSlope * minX + m_Offset, kSlope * maxX + m_Offset};
    }

private:
    float32 m_Offset;
};
} // namespace

#if defined(GE_PROBE_REFUSED)
GE_REGISTER_TERRAIN_HEIGHT_FUNCTION(ProbeSlope, "ProbeRefusedSlope")
constexpr int kRegisterResult = 17;
#else
GE_REGISTER_TERRAIN_HEIGHT_FUNCTION(ProbeSlope, "ProbeSlope")
constexpr int kRegisterResult = 0;
#endif

extern "C"
{
GE_USERAPI uint32_t GE_UserModule_AbiVersion_v1() { return GE_USERMODULE_ABI_VERSION; }

GE_USERAPI void GE_UserModule_ToolchainFingerprint_v1(GE_ToolchainFingerprint* out)
{
    GE_FillToolchainFingerprint(out);
}

GE_USERAPI int GE_UserModule_Register_v1(const GE_UserModuleHostApi*) { return kRegisterResult; }

GE_USERAPI void GE_UserModule_Unregister_v1(const GE_UserModuleHostApi*) {}
}
