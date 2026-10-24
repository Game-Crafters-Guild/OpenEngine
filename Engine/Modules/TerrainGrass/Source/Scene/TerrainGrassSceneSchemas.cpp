#include "TerrainGrass/Scene/TerrainGrassSceneSchemas.h"

#include "AssetCore/GUID.h"
#include "Components/Terrain/TerrainGrass.h"
#include "ECS/ECSTemplates.h"
#include "Scene/SceneSchemaRegistry.h"
#include "Scene/SceneValue.h"
#include "TerrainGrass/TerrainGrassClamps.h"

#include <cctype>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

namespace GameEngine::Scene
{
namespace
{

static std::string FormatFloat(float v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
    return buf;
}

static void SerializeGuid(std::vector<std::string>& outLines,
                          std::string_view property,
                          const GUID& guid)
{
    if (guid.IsNull())
        return;
    outLines.push_back(std::string(property) + " = &{" + guid.ToString() + "}");
}

static bool ParseGuid(std::string_view value, GUID& outGuid, std::string* outError)
{
    SceneValue v{};
    if (!ParseValue(value, v, outError))
        return false;

    std::string guidText;
    if (v.Kind == SceneValueKind::GuidRef)
        guidText = v.StringValue;
    else if (v.Kind == SceneValueKind::String)
        guidText = v.StringValue;
    else if (v.Kind == SceneValueKind::AssetRef)
        guidText = std::string(AssetRefGuid(v));
    else
    {
        if (outError)
            *outError = "Expected a GUID reference";
        return false;
    }

    outGuid = guidText.empty() ? GUID::Null() : GUID(guidText);
    return true;
}

static bool ParseF32(std::string_view value, float32& out, std::string* outError)
{
    SceneValue v{};
    if (!ParseValue(value, v, outError))
        return false;
    if (v.Kind == SceneValueKind::Float)
    {
        out = static_cast<float32>(v.FloatValue);
        return true;
    }
    if (v.Kind == SceneValueKind::Int)
    {
        out = static_cast<float32>(v.IntValue);
        return true;
    }
    if (outError)
        *outError = "Expected a numeric value";
    return false;
}

static bool ParseU32(std::string_view value, uint32& out, std::string* outError)
{
    SceneValue v{};
    if (!ParseValue(value, v, outError))
        return false;
    if (v.Kind == SceneValueKind::Int)
    {
        if (v.IntValue < 0 || v.IntValue > 0xFFFFFFFFll)
        {
            if (outError)
                *outError = "Value out of range for uint32";
            return false;
        }
        out = static_cast<uint32>(v.IntValue);
        return true;
    }
    if (v.Kind == SceneValueKind::Float)
    {
        out = static_cast<uint32>(v.FloatValue);
        return true;
    }
    if (outError)
        *outError = "Expected an integer value";
    return false;
}

static bool ParseBool(std::string_view value, bool& out, std::string* outError)
{
    SceneValue v{};
    if (!ParseValue(value, v, outError))
        return false;
    if (v.Kind != SceneValueKind::Bool)
    {
        if (outError)
            *outError = "Expected true/false";
        return false;
    }
    out = v.BoolValue;
    return true;
}

// An enum value as a scene writes it, without surrounding blanks or quotes and in lower case, so
// the parsers below compare names case-insensitively and quote-tolerantly.
static std::string NormalizeEnumToken(std::string_view value)
{
    std::string v(value);
    auto isTrim = [](char ch) { return ch == ' ' || ch == '\t' || ch == '"' || ch == '\''; };
    while (!v.empty() && isTrim(v.front())) v.erase(v.begin());
    while (!v.empty() && isTrim(v.back())) v.pop_back();
    for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return v;
}

// Parses a TerrainGrass render mode: the enum NAME (Dither / Blend) or a bare integer (0 / 1).
// Case-insensitive and quote-tolerant.
static bool ParseRenderMode(std::string_view value, Components::TerrainGrassRenderMode& out,
                            std::string* outError)
{
    const std::string v = NormalizeEnumToken(value);

    if (v == "dither" || v == "0") { out = Components::TerrainGrassRenderMode::Dither; return true; }
    if (v == "blend" || v == "1") { out = Components::TerrainGrassRenderMode::Blend; return true; }
    if (outError) *outError = "renderMode must be dither/blend or 0/1";
    return false;
}

// Parses a TerrainGrass preset: the enum NAME (Custom / Meadow / Lawn / Steppe) or a bare integer
// (0 to 3). Case-insensitive and quote-tolerant.
static bool ParsePreset(std::string_view value, Components::TerrainGrassPreset& out, std::string* outError)
{
    const std::string v = NormalizeEnumToken(value);

    if (v == "custom" || v == "0") { out = Components::TerrainGrassPreset::Custom; return true; }
    if (v == "meadow" || v == "1") { out = Components::TerrainGrassPreset::Meadow; return true; }
    if (v == "lawn" || v == "2") { out = Components::TerrainGrassPreset::Lawn; return true; }
    if (v == "steppe" || v == "3") { out = Components::TerrainGrassPreset::Steppe; return true; }
    if (outError) *outError = "preset must be custom/meadow/lawn/steppe or 0 to 3";
    return false;
}

static const char* PresetName(Components::TerrainGrassPreset preset)
{
    switch (preset)
    {
    case Components::TerrainGrassPreset::Meadow: return "Meadow";
    case Components::TerrainGrassPreset::Lawn: return "Lawn";
    case Components::TerrainGrassPreset::Steppe: return "Steppe";
    case Components::TerrainGrassPreset::Custom: break;
    }
    return "Custom";
}

class TerrainGrassSchema final : public ISceneComponentSchema
{
public:
    std::string_view GetComponentName() const override { return "TerrainGrass"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity,
                   [[maybe_unused]] const SceneSaveContext& ctx,
                   std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::TerrainGrass>(entity);
        if (!c)
            return;

        outLines.push_back(std::string("TerrainGrass.renderMode = ") +
            (c->RenderMode == Components::TerrainGrassRenderMode::Blend ? "Blend" : "Dither"));
        // A label of where the dials came from: a load restores it as written and checks nothing
        // against it.
        outLines.push_back(std::string("TerrainGrass.preset = ") + PresetName(c->Preset));
        outLines.push_back("TerrainGrass.bladesPerSquareMeter = " + FormatFloat(c->BladesPerSquareMeter));
        outLines.push_back("TerrainGrass.range = " + FormatFloat(c->Range));
        outLines.push_back("TerrainGrass.densityFalloff = " + FormatFloat(c->DensityFalloff));
        outLines.push_back("TerrainGrass.placementSeed = " + FormatFloat(c->PlacementSeed));
        outLines.push_back("TerrainGrass.clumpSize = " + FormatFloat(c->ClumpSize));
        outLines.push_back("TerrainGrass.clumpHeightVariance = " + FormatFloat(c->ClumpHeightVariance));
        outLines.push_back("TerrainGrass.clumpAlignment = " + FormatFloat(c->ClumpAlignment));
        outLines.push_back("TerrainGrass.clumpTaper = " + FormatFloat(c->ClumpTaper));
        outLines.push_back("TerrainGrass.bladeHeight = " + FormatFloat(c->BladeHeight));
        outLines.push_back("TerrainGrass.bladeWidth = " + FormatFloat(c->BladeWidth));
        outLines.push_back("TerrainGrass.bladeTilt = " + FormatFloat(c->BladeTilt));
        outLines.push_back("TerrainGrass.bladeBend = " + FormatFloat(c->BladeBend));
        outLines.push_back("TerrainGrass.bladeTwist = " + FormatFloat(c->BladeTwist));
        outLines.push_back("TerrainGrass.bladeTaperStart = " + FormatFloat(c->BladeTaperStart));
        outLines.push_back("TerrainGrass.tuftFan = " + FormatFloat(c->TuftFan));
        outLines.push_back("TerrainGrass.bladeSegments = " + std::to_string(c->BladeSegments));
        outLines.push_back("TerrainGrass.randomScale = " + FormatFloat(c->RandomScale));
        outLines.push_back("TerrainGrass.layerIndex = " + std::to_string(c->LayerIndex));
        outLines.push_back("TerrainGrass.maskThreshold = " + FormatFloat(c->MaskThreshold));
        outLines.push_back("TerrainGrass.windDirection = " + FormatFloat(c->WindDirection));
        outLines.push_back("TerrainGrass.windGustSpeed = " + FormatFloat(c->WindGustSpeed));
        outLines.push_back("TerrainGrass.windGustScale = " + FormatFloat(c->WindGustScale));
        outLines.push_back("TerrainGrass.windStrength = " + FormatFloat(c->WindStrength));
        outLines.push_back("TerrainGrass.windRestingLean = " + FormatFloat(c->WindRestingLean));
        outLines.push_back("TerrainGrass.windFlutterAmount = " + FormatFloat(c->WindFlutterAmount));
        outLines.push_back("TerrainGrass.windFlutterSpeed = " + FormatFloat(c->WindFlutterSpeed));
        outLines.push_back("TerrainGrass.windSeed = " + FormatFloat(c->WindSeed));
        outLines.push_back("TerrainGrass.brightness = " + FormatFloat(c->Brightness));
        outLines.push_back("TerrainGrass.randomBrightness = " + FormatFloat(c->RandomBrightness));
        outLines.push_back("TerrainGrass.hueVariation = " + FormatFloat(c->HueVariation));
        outLines.push_back("TerrainGrass.rootFadeStart = " + FormatFloat(c->RootFadeStart));
        outLines.push_back("TerrainGrass.rootFadeEnd = " + FormatFloat(c->RootFadeEnd));
        outLines.push_back("TerrainGrass.groundingStrength = " + FormatFloat(c->GroundingStrength));
        outLines.push_back("TerrainGrass.groundTintHeight = " + FormatFloat(c->GroundTintHeight));
        outLines.push_back("TerrainGrass.canopyShade = " + FormatFloat(c->CanopyShade));
        outLines.push_back("TerrainGrass.groundDarkening = " + FormatFloat(c->GroundDarkening));
        outLines.push_back("TerrainGrass.translucency = " + FormatFloat(c->Translucency));
        outLines.push_back(std::string("TerrainGrass.textureGrass = ") + (c->TextureGrass ? "true" : "false"));
        outLines.push_back("TerrainGrass.textureCardsPerSquareMeter = " + FormatFloat(c->TextureCardsPerSquareMeter));
        outLines.push_back("TerrainGrass.textureSize = " + FormatFloat(c->TextureSize));
        outLines.push_back(std::string("TerrainGrass.useSplatRootColor = ") + (c->UseSplatRootColor ? "true" : "false"));
        outLines.push_back("TerrainGrass.rootColor = " + std::to_string(c->RootColor));
        outLines.push_back("TerrainGrass.tipColor = " + std::to_string(c->TipColor));
        outLines.push_back("TerrainGrass.backlightColor = " + std::to_string(c->BacklightColor));
        outLines.push_back("TerrainGrass.soilColor = " + std::to_string(c->SoilColor));
        SerializeGuid(outLines, "TerrainGrass.albedoTexture", c->AlbedoTextureAssetGuid.ToGuid());
        SerializeGuid(outLines, "TerrainGrass.alphaTexture", c->AlphaTextureAssetGuid.ToGuid());
        SerializeGuid(outLines, "TerrainGrass.normalTexture", c->NormalTextureAssetGuid.ToGuid());
        outLines.push_back("TerrainGrass.atlasColumns = " + std::to_string(c->AtlasColumns));
        outLines.push_back("TerrainGrass.atlasRows = " + std::to_string(c->AtlasRows));
        outLines.push_back("TerrainGrass.atlasTileCount = " + std::to_string(c->AtlasTileCount));
        outLines.push_back("TerrainGrass.alphaCutoff = " + FormatFloat(c->AlphaCutoff));
        outLines.push_back("TerrainGrass.normalStrength = " + FormatFloat(c->NormalStrength));
    }

    bool ApplyProperty(ECS::World& world,
                       ECS::EntityHandle entity,
                       const SceneLoadContext& ctx,
                       std::string_view property,
                       std::string_view value,
                       std::string* outError) const override
    {
        Components::TerrainGrass c{};
        if (const auto* existing = world.GetComponent<Components::TerrainGrass>(entity))
            c = *existing;

        if (property == "rendermode") { if (!ParseRenderMode(value, c.RenderMode, outError)) return false; }
        else if (property == "preset") { if (!ParsePreset(value, c.Preset, outError)) return false; }
        else if (property == "bladespersquaremeter") { if (!ParseF32(value, c.BladesPerSquareMeter, outError)) return false; }
        else if (property == "range") { if (!ParseF32(value, c.Range, outError)) return false; }
        else if (property == "densityfalloff") { if (!ParseF32(value, c.DensityFalloff, outError)) return false; }
        else if (property == "placementseed") { if (!ParseF32(value, c.PlacementSeed, outError)) return false; }
        else if (property == "clumpsize") { if (!ParseF32(value, c.ClumpSize, outError)) return false; }
        else if (property == "clumpheightvariance") { if (!ParseF32(value, c.ClumpHeightVariance, outError)) return false; }
        else if (property == "clumpalignment") { if (!ParseF32(value, c.ClumpAlignment, outError)) return false; }
        else if (property == "clumptaper") { if (!ParseF32(value, c.ClumpTaper, outError)) return false; }
        else if (property == "bladeheight") { if (!ParseF32(value, c.BladeHeight, outError)) return false; }
        else if (property == "bladewidth") { if (!ParseF32(value, c.BladeWidth, outError)) return false; }
        else if (property == "bladetilt") { if (!ParseF32(value, c.BladeTilt, outError)) return false; }
        else if (property == "bladebend") { if (!ParseF32(value, c.BladeBend, outError)) return false; }
        else if (property == "bladetwist") { if (!ParseF32(value, c.BladeTwist, outError)) return false; }
        else if (property == "bladetaperstart") { if (!ParseF32(value, c.BladeTaperStart, outError)) return false; }
        else if (property == "tuftfan") { if (!ParseF32(value, c.TuftFan, outError)) return false; }
        else if (property == "bladesegments") { if (!ParseU32(value, c.BladeSegments, outError)) return false; }
        else if (property == "randomscale") { if (!ParseF32(value, c.RandomScale, outError)) return false; }
        else if (property == "layerindex") { if (!ParseU32(value, c.LayerIndex, outError)) return false; }
        else if (property == "maskthreshold") { if (!ParseF32(value, c.MaskThreshold, outError)) return false; }
        else if (property == "winddirection") { if (!ParseF32(value, c.WindDirection, outError)) return false; }
        else if (property == "windgustspeed") { if (!ParseF32(value, c.WindGustSpeed, outError)) return false; }
        else if (property == "windgustscale") { if (!ParseF32(value, c.WindGustScale, outError)) return false; }
        else if (property == "windstrength") { if (!ParseF32(value, c.WindStrength, outError)) return false; }
        else if (property == "windrestinglean") { if (!ParseF32(value, c.WindRestingLean, outError)) return false; }
        else if (property == "windflutteramount") { if (!ParseF32(value, c.WindFlutterAmount, outError)) return false; }
        else if (property == "windflutterspeed") { if (!ParseF32(value, c.WindFlutterSpeed, outError)) return false; }
        else if (property == "windseed") { if (!ParseF32(value, c.WindSeed, outError)) return false; }
        else if (property == "brightness") { if (!ParseF32(value, c.Brightness, outError)) return false; }
        else if (property == "randombrightness") { if (!ParseF32(value, c.RandomBrightness, outError)) return false; }
        else if (property == "huevariation") { if (!ParseF32(value, c.HueVariation, outError)) return false; }
        else if (property == "rootfadestart") { if (!ParseF32(value, c.RootFadeStart, outError)) return false; }
        else if (property == "rootfadeend") { if (!ParseF32(value, c.RootFadeEnd, outError)) return false; }
        else if (property == "groundingstrength") { if (!ParseF32(value, c.GroundingStrength, outError)) return false; }
        else if (property == "groundtintheight") { if (!ParseF32(value, c.GroundTintHeight, outError)) return false; }
        else if (property == "canopyshade") { if (!ParseF32(value, c.CanopyShade, outError)) return false; }
        else if (property == "grounddarkening") { if (!ParseF32(value, c.GroundDarkening, outError)) return false; }
        else if (property == "translucency") { if (!ParseF32(value, c.Translucency, outError)) return false; }
        else if (property == "texturegrass") { if (!ParseBool(value, c.TextureGrass, outError)) return false; }
        else if (property == "texturecardspersquaremeter") { if (!ParseF32(value, c.TextureCardsPerSquareMeter, outError)) return false; }
        else if (property == "texturesize") { if (!ParseF32(value, c.TextureSize, outError)) return false; }
        else if (property == "usesplatrootcolor") { if (!ParseBool(value, c.UseSplatRootColor, outError)) return false; }
        else if (property == "rootcolor") { if (!ParseU32(value, c.RootColor, outError)) return false; }
        else if (property == "tipcolor") { if (!ParseU32(value, c.TipColor, outError)) return false; }
        else if (property == "backlightcolor") { if (!ParseU32(value, c.BacklightColor, outError)) return false; }
        else if (property == "soilcolor") { if (!ParseU32(value, c.SoilColor, outError)) return false; }
        else if (property == "albedotexture") { GUID g{}; if (!ParseGuid(value, g, outError)) return false; c.AlbedoTextureAssetGuid.Set(g); }
        else if (property == "alphatexture") { GUID g{}; if (!ParseGuid(value, g, outError)) return false; c.AlphaTextureAssetGuid.Set(g); }
        else if (property == "normaltexture") { GUID g{}; if (!ParseGuid(value, g, outError)) return false; c.NormalTextureAssetGuid.Set(g); }
        else if (property == "atlascolumns") { if (!ParseU32(value, c.AtlasColumns, outError)) return false; }
        else if (property == "atlasrows") { if (!ParseU32(value, c.AtlasRows, outError)) return false; }
        else if (property == "atlastilecount") { if (!ParseU32(value, c.AtlasTileCount, outError)) return false; }
        else if (property == "alphacutoff") { if (!ParseF32(value, c.AlphaCutoff, outError)) return false; }
        else if (property == "normalstrength") { if (!ParseF32(value, c.NormalStrength, outError)) return false; }
        else
        {
            return ReportUnknownField(ctx, GetComponentName(), property, outError);
        }

        // The bounds are the component's, not this parser's, so an authored value and the same
        // value set over IPC land identically. Whole-component rather than per-property: it costs
        // one pass over a POD and it repairs a field an earlier writer left out of range.
        TerrainGrass::ClampFields(c);
        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::TerrainGrass>(entity))
            return true;
        world.AddComponentImmediate(entity, Components::TerrainGrass{});
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::TerrainGrass>(entity);
        return true;
    }
};

GE_REGISTER_SCENE_SCHEMA(TerrainGrassSchema)

static void RegisterIfMissing(std::unique_ptr<ISceneComponentSchema> schema)
{
    if (SceneSchemaRegistry::HasRegistered(schema->GetComponentName()))
        return;
    SceneSchemaRegistry::Register(std::move(schema));
}

} // namespace

void EnsureTerrainGrassSceneSchemasRegistered()
{
    static std::once_flag flag;
    std::call_once(flag, []() {
        RegisterIfMissing(std::make_unique<TerrainGrassSchema>());
    });
}

} // namespace GameEngine::Scene
