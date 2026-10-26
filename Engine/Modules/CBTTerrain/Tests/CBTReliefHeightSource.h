#pragma once

// A deterministic multi-octave relief height texture for the CBT real-device tests, and an RAII
// owner that binds it to every frame-params ring slot.
//
// Shared because the height regime is load-bearing for more than one suite: planar corners carry
// sampled height in .y (Kernel_VertexEval) scaled by TerrainSize[2] = heightScale, so at
// heightScale 0 every corner of a facet shares one y. Several geometric facts about LEB siblings
// are EXACT in that regime and only approximate with relief — a test that runs flat-only can assert
// the exact form and look green while saying nothing about production, which ships heightScale 60.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "CBTTerrain/CBTInstance.h"
#include "CBTTerrain/CBTLayout.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"

namespace GameEngine::CBTTerrain::Test
{

// Integer hash -> [0,1]. Deterministic across runs and platforms: the arms compare against
// measured bounds, so the relief must not move between runs.
inline float ReliefValueNoise(uint32_t x, uint32_t y)
{
    uint32_t h = x * 374761393u + y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<float>((h ^ (h >> 16)) & 0xFFFFFFu) / static_cast<float>(0xFFFFFF);
}

inline float ReliefSmoothNoiseAt(float u, float v, uint32_t period)
{
    const float su = u * static_cast<float>(period), sv = v * static_cast<float>(period);
    const uint32_t x0 = static_cast<uint32_t>(su), y0 = static_cast<uint32_t>(sv);
    const float fx = su - static_cast<float>(x0), fy = sv - static_cast<float>(y0);
    const float wx = fx * fx * (3.0f - 2.0f * fx), wy = fy * fy * (3.0f - 2.0f * fy);
    const float n00 = ReliefValueNoise(x0, y0), n10 = ReliefValueNoise(x0 + 1u, y0);
    const float n01 = ReliefValueNoise(x0, y0 + 1u), n11 = ReliefValueNoise(x0 + 1u, y0 + 1u);
    return (n00 * (1.0f - wx) + n10 * wx) * (1.0f - wy) + (n01 * (1.0f - wx) + n11 * wx) * wy;
}

// The relief's normalized height at a UV in [0, 1], the function MakeReliefHeightTexture samples at
// texel centers. Six octaves of value noise, so the height is non-linear along edges at every
// subdivision depth rather than only the coarse ones. A single smooth octave leaves the deep facets
// locally planar, which would understate exactly the residual these arms exist to measure.
inline float ReliefHeightAt(float u, float v)
{
    float amp = 0.5f, sum = 0.0f, norm = 0.0f;
    for (uint32_t octave = 0; octave < 6u; ++octave)
    {
        sum += amp * ReliefSmoothNoiseAt(u, v, 4u << octave);
        norm += amp;
        amp *= 0.5f;
    }
    return sum / norm;
}

// Copies `data` (row-major, width * height values) into an R32_FLOAT texture in state `prior`, leaves it
// in ShaderResource, and waits for the device.
inline void UploadR32Texture(Rendering::IDevice& device, Rendering::TextureHandle tex, uint32_t width, uint32_t height,
                             const std::vector<float>& data, Rendering::ResourceState prior)
{
    using namespace Rendering;
    const size_t bytes = data.size() * sizeof(float);
    const size_t rowPitch = static_cast<size_t>(width) * sizeof(float);
    BufferHandle staging = device.CreateUploadBuffer(bytes, "CBT.Test.R32Staging");
    device.UpdateBuffer(staging, 0, bytes, data.data());
    auto cl = device.CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->CopyBufferToTextureSubresource(staging, tex, 0, 0, width, height, 0, rowPitch, 1, 0, 0, 0, prior);
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(tex, ResourceState::CopyDest, ResourceState::ShaderResource));
    cl->End();
    std::vector<CommandList*> lists{cl.get()};
    device.ExecuteCommandLists(lists);
    device.WaitForIdle();
    device.DestroyBuffer(staging);
}

// A width x height R32_FLOAT texture a shader can sample, holding `data` (row-major). Empty `data` creates
// the texture and uploads nothing.
inline Rendering::TextureHandle MakeR32Texture(Rendering::IDevice& device, uint32_t width, uint32_t height,
                                               const std::vector<float>& data, const char* debugName)
{
    using namespace Rendering;
    TextureDesc td{};
    td.width = width;
    td.height = height;
    td.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource) |
               static_cast<uint32_t>(TextureUsage::TransferDst);
    td.persistent = true;
    td.debugName = debugName;
    TextureHandle tex = device.CreateTexture(td);
    if (tex.IsValid() && !data.empty())
        UploadR32Texture(device, tex, width, height, data, ResourceState::Undefined);
    return tex;
}

inline Rendering::TextureHandle MakeReliefHeightTexture(Rendering::IDevice& device, uint32_t dim)
{
    std::vector<float> data(static_cast<size_t>(dim) * dim);
    for (uint32_t y = 0; y < dim; ++y)
        for (uint32_t x = 0; x < dim; ++x)
        {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(dim);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(dim);
            data[static_cast<size_t>(y) * dim + x] = ReliefHeightAt(u, v);
        }
    return MakeR32Texture(device, dim, dim, data, "CBT.Test.Relief");
}

// Owns the relief texture for one arm and binds it to every ring slot, so any frame index samples
// the real relief rather than frame 0 only. A heightScale of 0 binds nothing: CBT_SampleHeight
// returns texel * terrainSize.z + terrainSize.w, so the sampled value cannot move a corner's y.
class ReliefHeightSource
{
  public:
    ReliefHeightSource() = default;
    ReliefHeightSource(Rendering::IDevice& device, CBTInstance& instance, float heightScale,
                       uint32_t dim)
    {
        if (heightScale == 0.0f)
            return;
        m_Texture = MakeReliefHeightTexture(device, dim);
        if (!m_Texture.IsValid())
            return;
        m_Device = &device;
        for (uint32_t slot = 0; slot < kCBTFrameParamsRing; ++slot)
            instance.SetHeightSource(slot, m_Texture);
    }
    ReliefHeightSource(const ReliefHeightSource&) = delete;
    ReliefHeightSource& operator=(const ReliefHeightSource&) = delete;
    ReliefHeightSource(ReliefHeightSource&& o) noexcept
        : m_Device(o.m_Device), m_Texture(o.m_Texture)
    {
        o.m_Device = nullptr;
    }
    ReliefHeightSource& operator=(ReliefHeightSource&&) = delete;
    ~ReliefHeightSource()
    {
        if (m_Device && m_Texture.IsValid())
            m_Device->DestroyTexture(m_Texture);
    }

    bool IsArmed() const { return m_Device != nullptr; }

  private:
    Rendering::IDevice* m_Device = nullptr;
    Rendering::TextureHandle m_Texture;
};

// The same relief through the atlas height source: one resident slot of tileRes x tileRes samples (sample
// i at UV i / (tileRes - 1), so the slot spans the terrain corner to corner) with a one-sample apron, and
// one indirection row. The atlas source keeps the screen-space edge rule (CBT_PlanarCriterionOn is false
// when AtlasParams1.z is 1), so a test arms the edge rule on relief by binding this and calling Apply on its
// frame params.
class ReliefAtlasSource
{
  public:
    ReliefAtlasSource(Rendering::IDevice& device, CBTInstance& instance, uint32_t tileRes) : m_TileRes(tileRes)
    {
        const uint32_t dim = tileRes + 2u;
        std::vector<float> atlas(static_cast<size_t>(dim) * dim);
        const float last = static_cast<float>(tileRes - 1u);
        for (uint32_t z = 0; z < dim; ++z)
            for (uint32_t x = 0; x < dim; ++x)
            {
                // The apron repeats the edge sample, so the bilinear tap at the slot's edge is clamped.
                const uint32_t sx = std::min(std::max(x, 1u), tileRes) - 1u;
                const uint32_t sz = std::min(std::max(z, 1u), tileRes) - 1u;
                atlas[static_cast<size_t>(z) * dim + x] =
                    ReliefHeightAt(static_cast<float>(sx) / last, static_cast<float>(sz) / last);
            }
        m_Texture = MakeR32Texture(device, dim, dim, atlas, "CBT.Test.ReliefAtlas");
        if (!m_Texture.IsValid())
            return;
        m_Device = &device;
        // One resident row: slot 0, generation 1, no LOD bias.
        const uint32_t row[4] = {0u, 1u, 0u, 0u};
        static_assert(sizeof(row) == kAtlasRowBytes);
        for (uint32_t slot = 0; slot < kCBTFrameParamsRing; ++slot)
        {
            instance.SetAtlasSource(slot, m_Texture);
            instance.UploadAtlasRows(slot, row, 1u);
        }
    }
    ReliefAtlasSource(const ReliefAtlasSource&) = delete;
    ReliefAtlasSource& operator=(const ReliefAtlasSource&) = delete;
    ~ReliefAtlasSource()
    {
        if (m_Device && m_Texture.IsValid())
            m_Device->DestroyTexture(m_Texture);
    }

    bool IsArmed() const { return m_Device != nullptr; }

    // The atlas geometry CBTRenderFeature writes for a one-tile atlas with the whole tile resident.
    void Apply(CBTFrameParams& p) const
    {
        const float dim = static_cast<float>(m_TileRes + 2u);
        p.AtlasParams0[0] = dim;
        p.AtlasParams0[1] = dim;
        p.AtlasParams0[2] = 1.0f;
        p.AtlasParams0[3] = static_cast<float>(m_TileRes);
        p.AtlasParams1[0] = 1.0f;
        p.AtlasParams1[1] = 1.0f;
        p.AtlasParams1[2] = 1.0f; // the atlas height source
        p.AtlasParams1[3] = 0.0f; // no coarse field: the tile is resident
    }

  private:
    Rendering::IDevice* m_Device = nullptr;
    Rendering::TextureHandle m_Texture;
    uint32_t m_TileRes = 0;
};

} // namespace GameEngine::CBTTerrain::Test
