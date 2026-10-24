#pragma once

#include "Rendering/Core/Handle.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

#include <cstdint>

namespace GameEngine::Engine::Renderer
{

// The terrain's sun-space clearance map (terrain_shadow.glsl) as the shading passes reach it.
// Published per view and frame by the terrain renderer to ShadowMapRenderFeature, which writes it
// into ShadowData for GE_SampleShadow; the ray-traced mask pass reads the same map. Positions are
// world units (metres) in the full world frame; the shadow feature rebases them to the view's
// render origin for the shading passes.
struct TerrainShadowMap
{
    // The map (RG16F: the clearance and the occluder distance, MapSide square, lines along v) as
    // imported into the publishing frame's render graph (valid in that frame only), its physical
    // texture and its bindless index.
    Rendering::RenderGraph::RGTexture Map{};
    Rendering::TextureHandle MapTexture{};
    uint32_t MapBindlessIndex = 0;
    uint32_t MapSide = 0;
    // The map's lit field (RG16F, half the map's side, a full mip chain): level 0 holds, over each 2 x 2
    // map samples, the share that sees the sun's center and the mean of their occluder octaves
    // (terrain_shadow_field_lit.comp); each next level averages 2 x 2 of the one below. A terrain pixel
    // that covers many samples reads it at its footprint's level, in both directional-shadow modes.
    // Invalid, 0 and 0 when not published: the receivers then read the map alone.
    Rendering::RenderGraph::RGTexture FieldLit{};
    uint32_t FieldLitBindlessIndex = 0;
    uint32_t FieldLitLevels = 0;
    // The terrain's height texture (one texel per lattice sample) and its bindless index.
    Rendering::TextureHandle HeightTexture{};
    uint32_t HeightBindlessIndex = 0;
    // The terrain's corner (world X, Z), the world height of normalized 0, its extent, and meters
    // per normalized height unit.
    double TerrainX = 0.0;
    double TerrainZ = 0.0;
    double BaseY = 0.0;
    float TerrainSizeX = 0.0f;
    float TerrainSizeZ = 0.0f;
    float HeightScale = 0.0f;
    // The grid (GE_TerrainShadowGrid): its center is the terrain's center; u runs along the sun's
    // horizontal direction (SunX, SunZ), v across it.
    float SunX = 0.0f;
    float SunZ = 0.0f;
    float TanElevation = 0.0f;
    float Texel = 0.0f;
    float UMin = 0.0f;
    float VMin = 0.0f;
    float SamplesU = 0.0f;
    float SamplesV = 0.0f;
    // The terrain draw's level scale for this view: a receiver d meters from the eye is drawn at the
    // continuous level log2(d x DrawLevelScale) (cbt_height_chain.glsl CBT_ChainLevel); 0 when the draw
    // has no level rule (no height chain), and then no receiver reads the lit field.
    float DrawLevelScale = 0.0f;
};

} // namespace GameEngine::Engine::Renderer
