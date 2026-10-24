#pragma once

#include "Components/AssetRef.h"
#include "Components/Terrain/Terrain.h" // TerrainDomain
#include "Types/Color.h"
#include "Types/Types.h"

#include <type_traits>

namespace GameEngine::Components
{

inline constexpr uint32 kMinTerrainGrassBladeSegments = 1u;
inline constexpr uint32 kMaxTerrainGrassBladeSegments = 12u;
// The blade shape dials' ranges (TerrainGrass::BladeTilt and the four after it): radians, radians,
// turns, a height fraction short of the tip, and radians per Clump Size of distance.
inline constexpr float32 kMaxTerrainGrassBladeTilt = 1.2f;
inline constexpr float32 kMaxTerrainGrassBladeBend = 1.5f;
inline constexpr float32 kMaxTerrainGrassBladeTwist = 2.0f;
inline constexpr float32 kMaxTerrainGrassBladeTaperStart = 0.95f;
inline constexpr float32 kMaxTerrainGrassTuftFan = 2.0f;
// The most light through a blade's tip, against the light its sunlit face reflects: as much through
// a thin leaf as off it (TerrainGrass::Translucency). Mirror of kGrassMaxTranslucency in
// terrain_grass_surface.glsl.
inline constexpr float32 kMaxTerrainGrassTranslucency = 1.0f;
// smoothstep is undefined on an empty span: the root fade's end is held this far above its start.
inline constexpr float32 kMinTerrainGrassRootFadeSpan = 0.01f;
// The gradient may start below the root: a negative start puts part of the tip colour on the
// root itself. -1 is one blade height below it.
inline constexpr float32 kMinTerrainGrassRootFadeStart = -1.0f;

// How blades resolve their soft alpha, which is the card texture's alpha and nothing else — a
// blade BODY is opaque. Geometric grass binds no texture, so it has no soft alpha for either mode
// to resolve and both draw solid blades whose only shape is their geometry.
//
// Dither: alpha-test class. Blades write and test depth, so blade-over-blade occlusion is
// depth-correct regardless of draw order, and blades occlude water/transparents behind them.
// The soft alpha becomes screen-space dithered coverage: under MSAA, alpha-to-coverage turns it
// into subsample coverage; without MSAA it is a screen-door pattern (resolved smooth by TAA).
//
// Blend: classic alpha blending. A texture card's soft edges stay smooth without any AA, but
// blades write no depth and blade-over-blade compositing follows draw order, not camera distance —
// the far LOD band is drawn first so it can never composite over near blades, but blades within a
// band blend in placement order. Geometric grass gains nothing from this and still pays that
// ordering: at alpha 1.0 the placement order reads as an occlusion order rather than a blend.
//
// The mode is authored per component; grass instances from every terrain share one draw per LOD
// band, so a view whose active grass terrains disagree resolves to Dither (the depth-correct
// choice) for all of them.
enum class TerrainGrassRenderMode : uint8
{
    Dither = 0,
    Blend = 1,
};

// The kind of field the palette and blade dials were set from (TerrainGrass/TerrainGrassPresets.h).
// Meadow is the component's defaults. Custom once any of the preset's dials is edited away from it.
enum class TerrainGrassPreset : uint8
{
    Custom = 0,
    Meadow = 1,
    Lawn = 2,
    Steppe = 3,
};

// Grass placement is planar-only. The placement compute (terrain_grass_place.comp) lays blade
// candidates on a world-anchored XZ cell lattice inside a camera-relative window and roots each at
// y = WorldOriginY + height, with blade up = the world-Y-dominant terrain normal. On a cube-sphere
// planet (TerrainDomain::Spherical) the surface is dir*(R + relief + sculpt), so those same
// candidates float off the curved shell, adrift above the planet. Radial
// (on-sphere) placement is a separate arc (see the planet-grass design doc); until it lands a
// spherical terrain sources NO grass rather than floaters. One predicate so the extraction gate and
// its oracle can never disagree on which domains place grass.
inline constexpr bool TerrainGrassSupportsDomain(TerrainDomain domain)
{
    return domain == TerrainDomain::Planar;
}

// Root Color's "Auto" value: no colour is authored, and the root of a blade's own gradient derives
// from Tip Color (TerrainGrassResolvedRootColor). Alpha 0 is never an authored colour: the inspector
// and the colour picker write opaque colours only.
inline constexpr uint32 kTerrainGrassAutoRootColor = 0u;

// How much of Tip Color's luminance an automatic root keeps where the blade has no ground colour
// to start from (Splat Root Color off); with it on, an automatic root starts from the ground colour
// at the blade's root, by Grounding Strength (GrassBladeCanopyRoot in terrain_grass_surface.glsl).
// The three grass colours are stored as sRGB bytes and the surface decodes them to linear albedo
// on read (grassColorFromArgb in terrain_grass_surface.glsl), so the rule runs in linear light:
// decode the tip, scale every channel by this, encode back to bytes. Scaling linear RGB scales
// luminance by exactly this and keeps hue and chromaticity: the root is the tip in shade, so the
// blade always darkens toward the ground whatever tip a scene authors. A fixed default root cannot
// do that: against a dark authored tip it is the lighter end and the gradient brightens toward the
// ground.
inline constexpr float32 kTerrainGrassAutoRootLuminance = 0.4f;

// The ARGB the root of a blade's gradient uses: the authored Root Color, or under Auto the tip
// darkened to kTerrainGrassAutoRootLuminance in linear light.
inline uint32 TerrainGrassAutoRootColor(uint32 tipColor)
{
    const ColorSRGB tip = ColorSRGB::FromSRGB255(static_cast<uint8>((tipColor >> 16) & 0xFFu),
                                                 static_cast<uint8>((tipColor >> 8) & 0xFFu),
                                                 static_cast<uint8>(tipColor & 0xFFu));
    ColorLinear root = tip.ToLinear();
    root.r *= kTerrainGrassAutoRootLuminance;
    root.g *= kTerrainGrassAutoRootLuminance;
    root.b *= kTerrainGrassAutoRootLuminance;
    const ColorSRGB encoded = root.ToSRGB();
    const auto toByte = [](float32 channel) { return static_cast<uint32>(channel * 255.0f + 0.5f); };
    return 0xFF000000u | (toByte(encoded.r) << 16) | (toByte(encoded.g) << 8) | toByte(encoded.b);
}

// @ge-no-add  Requires a Terrain on the same entity (only read inside Query<Terrain,...>); inert
// standalone, so it's reflected (MCP/snapshot + its custom inspector) but kept out of the Add menu.
struct TerrainGrass
{
    TerrainGrassRenderMode RenderMode = TerrainGrassRenderMode::Dither;
    // The kind of field the palette, blade size, density and tilt below come from. Picking one in
    // the inspector writes those dials (ApplyTerrainGrassPreset); they stay editable.
    TerrainGrassPreset Preset = TerrainGrassPreset::Meadow;
    // Blades per square metre AT THE CAMERA, independent of terrain size: the same value reads the
    // same on a 512 m and a 10 km terrain. Density falls off to zero at Range along DensityFalloff,
    // and blades leave by not being spawned, so there is no fade band.
    //
    // This density and this Range together over-subscribe the per-view instance budget, so the fit
    // shortens the RANGE and holds the near density (GrassFitPlacementToBudget). That is the
    // intended trade: density at the camera is what a field's look is judged on, distance is what
    // pays for it.
    //
    // Ceiling: 256 blades/m² (TerrainGrass::kGrassMaxBladesPerSquareMeter, the per-cell slot
    // limit). A denser value costs no memory, because the instance pool is a fixed per-view budget;
    // it costs range, which the fit gives up to hold the near density.
    float32 BladesPerSquareMeter = 21.0f;
    float32 Range = 500.0f;
    float32 DensityFalloff = 2.0f;
    float32 PlacementSeed = 3.0f;
    // Voronoi clumping. Blades are laid out by a low-discrepancy sequence, which is anti-clump by
    // construction — a field of it reads as mown rather than grown. A second, coarser, world-
    // anchored lattice groups blades into tufts that share a height, a facing and a colour. Edge
    // of a clump in metres; 0 or less turns the whole mechanism off and restores the plain field.
    //
    // A clump never moves a blade and never removes one: every part of the ground keeps the
    // authored density, and a tuft reads by its shape (ClumpTaper), its height, facing and hue.
    float32 ClumpSize = 1.1f;
    // How far a clump's height departs from the authored one, +/- this fraction. 0 = every clump
    // the same height as every other.
    float32 ClumpHeightVariance = 0.3f;
    // How far each blade's yaw is pulled toward its clump's common facing. 0 = independent yaw per
    // blade (the unclumped field), 1 = every blade in a tuft faces identically.
    float32 ClumpAlignment = 0.45f;
    // How much shorter a tuft's outer blades are than its centre blades: a blade's height is scaled
    // by 1 - ClumpTaper x smoothstep(0, 0.75, d), where d is its distance from the tuft's centre in
    // clump cells (0.75 is about the edge of a tuft). 0 = every blade of a tuft at the tuft's
    // height; 1 = the edge blades shrink to nothing, which bares the ground between tufts again.
    // Blade positions do not depend on it, so the density is the authored density everywhere.
    float32 ClumpTaper = 0.35f;
    float32 BladeHeight = 0.72f;
    // The whole leaf's width at its base, in metres. A meadow leaf is 3 to 8 mm wide on a 0.4 to
    // 0.8 m blade; the default is wider so the default 21 blades per square metre still cover the
    // ground from eye height and from a strategy camera. A game that places more blades, or looks
    // from close up, narrows it. The sides stay parallel up to BladeTaperStart, which is what gives
    // a narrow blade its coverage. Never wider than a fifth of the blade's height
    // (kGrassMaxBladeAspect in the vertex stage), so a short blade cannot become a square.
    float32 BladeWidth = 0.036f;
    // THE BLADE'S SHAPE: five physical dials. The spine's angle from the terrain normal at height
    // fraction t is BladeTilt + BladeBend t^2 + 0.6 BladeBend t^4: a tilted base, the cantilever
    // bend of a leaf under its own weight, and the tip's droop. Each blade draws its tilt and its
    // bend +/-40% about these, and its twist +/-50% in a random direction, from its site.
    //
    // Radians from vertical at the root (the inspector shows degrees). The default suits a meadow of
    // upright leaves; a lawn is near 0.1, a loose meadow 0.3 to 0.5.
    float32 BladeTilt = 0.25f;
    // Radians of bend over the blade's length (the tip droop adds 0.6 of this again). The wind
    // adds its own bend on top, along the wind.
    float32 BladeBend = 0.35f;
    // Turns of torsion from root to tip: a quarter turn turns the leaf's face through 90 degrees,
    // so its width and its shading change along its length. 0 is a flat ribbon.
    float32 BladeTwist = 0.25f;
    // Height fraction where the leaf stops being parallel-sided and tapers smoothly to its point.
    float32 BladeTaperStart = 0.6f;
    // Radians of lean per Clump Size of distance from the tuft's centre: a blade leans away from
    // its tuft's centre, the outer ones the most, so a tuft reads as a fan. 0 = no fan. Needs
    // ClumpSize above 0.
    float32 TuftFan = 0.25f;
    uint32 BladeSegments = 5u;
    float32 RandomScale = 0.35f;
    uint32 LayerIndex = 0;
    float32 MaskThreshold = 0.45f;
    float32 WindDirection = -2.5f;
    float32 WindGustSpeed = 0.9f;
    float32 WindGustScale = 0.05f;
    float32 WindStrength = 1.0f;
    float32 WindRestingLean = 0.12f;
    float32 WindFlutterAmount = 0.16f;
    float32 WindFlutterSpeed = 2.2f;
    float32 WindSeed = 3.0f;
    float32 Brightness = 1.0f;
    // Per-blade value jitter: a blade's canopy is scaled by 1 +/- this. It carries the field's
    // blade-to-blade TONAL separation, which is what a noon field with the sun behind the camera has
    // left to read by: there the ground and the grass are lit almost identically and the root is the
    // terrain's own colour by construction, so a blade's own value is all that distinguishes it from
    // its neighbour.
    //
    // The grain is the BLADE, not the pixel, so raising this cannot make pixel noise — it darkens
    // the darkest blades, smoothly and with no threshold anywhere in the range. The ceiling is
    // therefore taste rather than a defect boundary, and the trade to weigh is field variety
    // against how dark the darkest blade may be.
    //
    // The draw is the instance's per-blade random REHASHED (terrain_grass_surface.glsl), as the hue
    // draw below is, so a blade's brightness does not follow its width: a twisted blade changes width
    // along its length, and a coupling between the two would read as a rule.
    float32 RandomBrightness = 0.42f;
    // Hue jitter, in [0,1] where 1 is a 30-degree swing either way between whole tufts. Value-only
    // variation (the two Brightness knobs above) makes a field read as one colour lit unevenly; a
    // little hue spread is what makes it read as many plants. Blades inside a tuft break up against
    // each other with a fixed share of the same amount (kGrassBladeHueShare in grass_clump.glsl),
    // so this is the one dial for "how many different greens is this field", and the two
    // granularities sum: at 0.45 a tuft moves +/-13.5 degrees and a blade inside it +/-18.9.
    //
    // NEARLY free in brightness, not exactly: GrassHueShift rotates about (1,1,1)/sqrt(3), which
    // preserves R+G+B and the vector's length but NOT Rec.709 luma, so a rotation does move a
    // blade's value a little (a few 8-bit levels at this default on the shipped tip colour), and
    // the negative-albedo clamp after it is a further one-way change at large angles. Its ceiling
    // is nevertheless how much spread still reads as ONE plant rather than any brightness cost:
    // what grows with the dial is the spread between neighbouring blades, and far enough up that
    // spread reads as two species mixed rather than one sward. "Still a plausible green" is a
    // statement about the SHIPPED palette — a bound albedo texture multiplies in before this
    // rotation, so a textured blade's hue excursion is whatever its texture makes it.
    float32 HueVariation = 0.45f;
    // THE GROUND HANDOVER: the height fractions between which a blade leaves the ground colour it
    // grows out of and becomes its own canopy colour. Below RootFadeStart the blade IS the terrain;
    // from RootFadeEnd up it is entirely itself. The canopy's own RootColor-to-TipColor gradient runs
    // the whole blade whatever this span is, so a short span cannot flatten the upper blade. A
    // negative start begins the blend below the root, so the root already carries part of the tip
    // colour. This is a material lever over the blade's albedo
    // only: the shading normal still settles onto the terrain normal over the whole blade, whatever
    // span is authored. The shipped span ends at 0.275 of the blade's height: over a shorter one
    // the blade's face-on reflection (kGrassReflectionHandoverSharpness in
    // terrain_grass_surface.glsl) leaves the ground's too abruptly to keep the contact.
    //
    // The ground colour is ONE colour per blade: the terrain's albedo resolved at the blade's ROOT
    // over a 1 m footprint (terrain_grass_vertex_modifier.glsl, kGrassRootGroundFootprint), carried
    // to every fragment of the blade. It cannot draw the ground's pattern up the blade. The blade's
    // root is therefore the ground's local average colour, not the exact texel beside it.
    float32 RootFadeStart = 0.0f;
    float32 RootFadeEnd = 0.275f;
    // The master gate over the whole GROUNDING read, and nothing to do with cast shadows: it scales
    // the wind-gust shading and the blade's blend into the ground colour it
    // grows out of (the root colour above, over the RootFade span). At 0 a blade never adopts its
    // terrain's colour at all, which destroys the contact identity the rest of this component is
    // built around, so it is an escape hatch for a deliberately stylised field rather than a look
    // dial.
    float32 GroundingStrength = 1.0f;
    // How much light the field keeps from its own lower blades and from the soil under it, sun and
    // sky alike. 1 is the physical amount for the authored density, width and height: under a
    // field the ground is darker than open ground and the blade base stays the ground's colour, so
    // the lit tips are the brightest part of the field. 0 lights the ground under the grass as open
    // ground (a bright lawn); above 1 the field is shaded more, up to the depth cap. The blade base and the ground beside it
    // always take the same shade (Includes/terrain_grass_canopy.glsl).
    float32 CanopyShade = 1.0f;
    // How far the ground under this field is mixed toward SoilColor, 0 to 1. The bake writes
    // SoilColor into the terrain's colour map where the blades stand, at this weight times the
    // field's coverage and any Grass Area region's Darken Ground scale, so the ground under a field
    // reads as soil beside the lit blades. 0 writes nothing. A planar terrain only; applies at the
    // next bake, not the same frame.
    float32 GroundDarkening = 0.0f;
    // How much light passes through a blade's tip, against the light its sunlit face reflects: a
    // face turned away from the sun is lit by the light through the leaf, and a field seen against
    // the sun glows at its tips. 1 is a thin leaf, as much light through it as off it, and the most:
    // a back-lit blade is never brighter than the same blade lit from the front. 0 makes the blade
    // opaque. The default 0.7 is a grass leaf, which passes less light than it reflects: under the
    // sky both faces of a blade gather light, so at 1 a tip is twice as bright as its colour and a
    // field under a low sun reads with pale, near-white tips. The light through falls to nothing
    // toward the thick root. Cast shadows dim it like any other light. This is the lighting model's
    // diffuse transmission (MaterialKeyword::DiffuseTransmission): a share of the light on one face
    // leaves the other, inside the light's shadow. The Subsurface lobe does not apply: it adds a
    // view-dependent glow outside the shadow multiply, so a blade would be brighter against the sun
    // than lit from the front, and a field in a hill's shadow would still glow.
    float32 Translucency = 0.7f;
    bool TextureGrass = false;
    float32 TextureCardsPerSquareMeter = 1.5f;
    float32 TextureSize = 1.0f;
    bool UseSplatRootColor = true;
    // THE COLOUR CONTRACT for RootColor, TipColor and BacklightColor: sRGB bytes packed 0xAARRGGBB,
    // the bytes the colour picker writes and the inspector swatch shows. The surface decodes each to
    // linear albedo on read (grassColorFromArgb), so the swatch is the blade's colour before
    // lighting.
    //
    // The root of each blade's own gradient. kTerrainGrassAutoRootColor (the default) starts from the
    // ground colour at the blade's root with UseSplatRootColor, by GroundingStrength, else it is
    // derived from TipColor; any opaque colour is used as authored. Read the uploaded colour through
    // TerrainGrassResolvedRootColor, never directly.
    uint32 RootColor = kTerrainGrassAutoRootColor;
    // A leaf's albedo, linear (0.19, 0.30, 0.075): a meadow green that a lit tip carries above the
    // ground it grows in. sRGB bytes, as the picker writes them; the surface decodes them
    // (grassColorFromArgb).
    uint32 TipColor = 0xFF79954Du;
    // The colour of the light through a blade: its hue and saturation. Its brightness is the
    // blade's own (the root-to-tip tone, the brightness jitter and the canopy shade carry over), so
    // only the colour of the glow is authored here. Linear (0.50, 0.62, 0.12): light through a green
    // leaf is a more yellow green.
    uint32 BacklightColor = 0xFFBCCE61u;
    // The colour GroundDarkening mixes the ground under the field toward, sRGB bytes as Tip Color.
    // Linear (0.10, 0.075, 0.045): a dark brown soil.
    uint32 SoilColor = 0xFF594D3Cu;
    Components::TextureRef AlbedoTextureAssetGuid;
    Components::TextureRef AlphaTextureAssetGuid;
    Components::TextureRef NormalTextureAssetGuid;
    uint32 AtlasColumns = 1u;
    uint32 AtlasRows = 1u;
    uint32 AtlasTileCount = 1u;
    float32 AlphaCutoff = 0.35f;
    float32 NormalStrength = 0.75f;
};

inline uint32 TerrainGrassResolvedRootColor(const TerrainGrass& grass)
{
    return grass.RootColor == kTerrainGrassAutoRootColor ? TerrainGrassAutoRootColor(grass.TipColor)
                                                         : grass.RootColor;
}

static_assert(std::is_trivially_copyable_v<TerrainGrass>,
              "TerrainGrass must be trivially copyable for ECS storage");
static_assert(std::is_standard_layout_v<TerrainGrass>,
              "TerrainGrass must be standard layout for ECS storage");

} // namespace GameEngine::Components
