#pragma once

// Shared helpers for the CBTTerrain real-device tests. Mirrors the headless
// Vulkan device bring-up used by the Rendering module's DeviceDiagnosticsTests.

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

#include "CBTTerrain/CBTDeepDecode.h"
#include "CBTTerrain/CBTLayout.h"
#include "Mathematics/WorldPosition.h"
#include "Rendering/Core/Device.h"

namespace GameEngine::CBTTerrain::Test
{

inline void SetEnvVar(const char* key, const char* value)
{
#if defined(_WIN32)
    _putenv_s(key, value);
#else
    setenv(key, value, 1);
#endif
}

inline void UnsetEnvVar(const char* key)
{
#if defined(_WIN32)
    _putenv_s(key, ""); // an empty value removes the variable
#else
    unsetenv(key);
#endif
}

inline void SetHeadlessEnv()
{
    SetEnvVar("GE_HEADLESS_TEST", "1");
}

inline std::unique_ptr<Rendering::IDevice> MakeHeadlessDevice()
{
    using namespace Rendering;
    SetHeadlessEnv();
    DeviceDesc dd{};
    dd.preferredAPI = GraphicsAPI::Vulkan;
    dd.enableDynamicRendering = true;
    auto device = DeviceFactory::CreateDevice(dd);
    if (!device || !device->Initialize(dd))
        return nullptr;
    return device;
}

inline std::filesystem::path ShaderOutputDir()
{
    return std::filesystem::path(CBT_SHADER_OUTPUT_DIR);
}

// World position (m) of one corner of a CurrentVertex slot. A slot with DeepTag 1 holds a
// sector-local offset: world = sector * 1024 + local, exact in double.
// A slot with DeepTag 0 (planar terrain, or a sphere on the narrow arm) holds the fp32 world position.
inline void CornerWorld(const CBTVertexData& vertex, uint32_t corner, double out[3])
{
    const float* const locals[] = {vertex.Corner0, vertex.Corner1, vertex.Corner2};
    const float* local = locals[corner];
    if (vertex.DeepTag[0] == 0u)
    {
        for (uint32_t axis = 0; axis < 3u; ++axis)
            out[axis] = local[axis];
        return;
    }
    const uint32_t* const sectors[] = {vertex.Sector0, vertex.Sector1, vertex.Sector2};
    const uint32_t* words = sectors[corner];
    const int32_t sector[3] = {UnpackSectorLo(words[0]), UnpackSectorHi(words[0]), UnpackSectorLo(words[1])};
    for (uint32_t axis = 0; axis < 3u; ++axis)
        out[axis] = Mathematics::kWorldSectorSize * sector[axis] + double(local[axis]);
}

// CornerWorld for one slot of a CurrentVertex buffer read back as words.
inline void ReadCornerWorld(const std::vector<uint32_t>& vertexWords, uint32_t slot, uint32_t corner,
                            double out[3])
{
    std::array<uint32_t, sizeof(CBTVertexData) / sizeof(uint32_t)> recordWords;
    std::copy_n(vertexWords.data() + size_t(slot) * recordWords.size(), recordWords.size(), recordWords.data());
    const CBTVertexData vertex = std::bit_cast<CBTVertexData>(recordWords);
    CornerWorld(vertex, corner, out);
}

// CurrentVertex words read as floats, with every corner's xyz the fp32 world position (m).
// For tests that read corner positions from the float view of the vertex buffer.
inline std::vector<float> VertexFloatsWithWorldCorners(const std::vector<uint32_t>& vertexWords)
{
    constexpr uint32_t kWordsPerSlot = sizeof(CBTVertexData) / 4u;
    constexpr uint32_t kCornerWords = sizeof(CBTVertexData::Corner0) / 4u;
    std::vector<float> floats(vertexWords.size());
    std::memcpy(floats.data(), vertexWords.data(), vertexWords.size() * sizeof(uint32_t));
    const uint32_t slotCount = static_cast<uint32_t>(vertexWords.size()) / kWordsPerSlot;
    for (uint32_t slot = 0; slot < slotCount; ++slot)
    {
        for (uint32_t corner = 0; corner < 3u; ++corner)
        {
            double world[3];
            ReadCornerWorld(vertexWords, slot, corner, world);
            for (uint32_t axis = 0; axis < 3u; ++axis)
                floats[slot * kWordsPerSlot + corner * kCornerWords + axis] = static_cast<float>(world[axis]);
        }
    }
    return floats;
}

} // namespace GameEngine::CBTTerrain::Test
