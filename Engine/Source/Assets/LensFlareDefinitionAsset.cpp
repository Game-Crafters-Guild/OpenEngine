#include "Assets/LensFlareDefinitionAsset.h"

#include "AssetCore/SharedFileRead.h"
#include "Logger/Logger.h"
#include "Serialization/JsonRead.h"

#include <format>
#include <fstream>

#include <nlohmann/json.hpp>

namespace GameEngine {

namespace {

using Serialization::JsonFieldReader;

// A colour object's channels over `fallback`; a value that is not an object is the fallback.
// Field names in refusals start with `context`.
LensFlare::Color ParseColor(const nlohmann::json& j, LensFlare::Color fallback, JsonFieldReader& reader,
                            std::string context)
{
    if (!j.is_object())
        return fallback;
    reader.SetContext(std::move(context));
    LensFlare::Color c = fallback;
    reader.Read(j, "r", c.R);
    reader.Read(j, "g", c.G);
    reader.Read(j, "b", c.B);
    reader.Read(j, "a", c.A);
    return c;
}

nlohmann::json ColorToJson(const LensFlare::Color& c)
{
    return {{"r", c.R}, {"g", c.G}, {"b", c.B}, {"a", c.A}};
}

nlohmann::json CurveToJson(const std::vector<LensFlare::CurveKeyData>& keys)
{
    nlohmann::json out = nlohmann::json::array();
    for (const LensFlare::CurveKeyData& key : keys)
    {
        out.push_back({{"time", key.Time},
                       {"value", key.Value},
                       {"inTangent", key.InTangent},
                       {"outTangent", key.OutTangent}});
    }
    return out;
}

// The curve `name` of `parent`, when it is an array: each key an object of time, value and tangents.
void ParseCurve(const nlohmann::json& parent, const char* name, std::vector<LensFlare::CurveKeyData>& out,
                JsonFieldReader& reader)
{
    auto it = parent.find(name);
    if (it == parent.end() || !it->is_array())
        return;
    out.reserve(it->size());
    for (size_t index = 0; index < it->size(); ++index)
    {
        reader.SetContext(std::format("globals.{}[{}].", name, index));
        const nlohmann::json& k = (*it)[index];
        LensFlare::CurveKeyData key;
        reader.Read(k, "time", key.Time);
        reader.Read(k, "value", key.Value);
        reader.Read(k, "inTangent", key.InTangent);
        reader.Read(k, "outTangent", key.OutTangent);
        out.push_back(key);
    }
}

void ParseGlobals(const nlohmann::json& j, LensFlare::FlareGlobals& g, JsonFieldReader& reader)
{
    reader.SetContext("globals.");
    reader.Read(j, "globalScale", g.GlobalScale);
    reader.Read(j, "globalBrightness", g.GlobalBrightness);
    if (j.contains("globalTint"))
        g.GlobalTint = ParseColor(j["globalTint"], g.GlobalTint, reader, "globals.globalTint.");

    reader.SetContext("globals.");
    reader.Read(j, "useDistanceFade", g.UseDistanceFade);
    reader.Read(j, "useDistanceScale", g.UseDistanceScale);
    reader.Read(j, "useMaxDistance", g.UseMaxDistance);
    reader.Read(j, "maxDistance", g.MaxDistance);

    reader.Read(j, "useAngleLimit", g.UseAngleLimit);
    reader.Read(j, "maxAngle", g.MaxAngle);
    reader.Read(j, "useAngleScale", g.UseAngleScale);
    reader.Read(j, "useAngleBrightness", g.UseAngleBrightness);
    reader.Read(j, "useAngleCurve", g.UseAngleCurve);

    ParseCurve(j, "angleCurve", g.AngleCurveKeys, reader);
    ParseCurve(j, "dynamicEdgeCurve", g.DynamicEdgeCurveKeys, reader);

    reader.SetContext("globals.");
    reader.Read(j, "offScreenFadeDist", g.OffScreenFadeDist);

    reader.Read(j, "useDynamicEdgeBoost", g.UseDynamicEdgeBoost);
    reader.Read(j, "dynamicEdgeBrightness", g.DynamicEdgeBrightness);
    reader.Read(j, "dynamicEdgeRange", g.DynamicEdgeRange);
    reader.Read(j, "dynamicEdgeBias", g.DynamicEdgeBias);

    reader.Read(j, "useDynamicEdgeScale", g.UseDynamicEdgeScale);
    reader.Read(j, "dynamicEdgeScale", g.DynamicEdgeScale);
    reader.Read(j, "dynamicEdgeScaleRange", g.DynamicEdgeScaleRange);
    reader.Read(j, "dynamicEdgeScaleBias", g.DynamicEdgeScaleBias);

    reader.Read(j, "useDynamicCenterBoost", g.UseDynamicCenterBoost);
    reader.Read(j, "dynamicCenterBrightness", g.DynamicCenterBrightness);
    reader.Read(j, "dynamicCenterScale", g.DynamicCenterScale);
    reader.Read(j, "dynamicCenterRange", g.DynamicCenterRange);
    reader.Read(j, "dynamicCenterBias", g.DynamicCenterBias);

    reader.Read(j, "multiplyScaleByTransformScale", g.MultiplyScaleByTransformScale);

    reader.Read(j, "neverCull", g.NeverCull);
}

LensFlare::FlareElement ParseElement(const nlohmann::json& j, size_t index, JsonFieldReader& reader)
{
    const std::string context = std::format("elements[{}].", index);
    reader.SetContext(context);
    LensFlare::FlareElement e;
    reader.Read(j, "sprite", e.SpriteName);
    reader.Read(j, "visible", e.Visible);
    reader.Read(j, "brightness", e.Brightness);
    reader.Read(j, "scale", e.Scale);
    reader.Read(j, "sizeX", e.SizeX);
    reader.Read(j, "sizeY", e.SizeY);
    reader.Read(j, "position", e.Position);
    reader.Read(j, "offsetX", e.OffsetX);
    reader.Read(j, "offsetY", e.OffsetY);
    reader.Read(j, "anamorphicX", e.AnamorphicX);
    reader.Read(j, "anamorphicY", e.AnamorphicY);
    reader.Read(j, "angle", e.Angle);
    reader.Read(j, "useStarRotation", e.UseStarRotation);
    reader.Read(j, "rotateToFlare", e.RotateToFlare);
    reader.Read(j, "rotationSpeed", e.RotationSpeed);
    if (j.contains("tint"))
        e.Tint = ParseColor(j["tint"], e.Tint, reader, context + "tint.");
    // Absent = negative default = inherit the flare's global boost.
    reader.SetContext(context);
    reader.Read(j, "edgeBrightnessBoost", e.EdgeBrightnessBoost);
    reader.Read(j, "centerBrightnessBoost", e.CenterBrightnessBoost);
    reader.Read(j, "edgeScaleBoost", e.EdgeScaleBoost);
    reader.Read(j, "centerScaleBoost", e.CenterScaleBoost);
    return e;
}

} // namespace

bool LensFlareDefinitionAsset::Save() const
{
    const LensFlare::FlareGlobals& g = m_Globals;
    nlohmann::json globals{
        {"globalScale", g.GlobalScale},
        {"globalBrightness", g.GlobalBrightness},
        {"globalTint", ColorToJson(g.GlobalTint)},
        {"useDistanceFade", g.UseDistanceFade},
        {"useDistanceScale", g.UseDistanceScale},
        {"useMaxDistance", g.UseMaxDistance},
        {"maxDistance", g.MaxDistance},
        {"useAngleLimit", g.UseAngleLimit},
        {"maxAngle", g.MaxAngle},
        {"useAngleScale", g.UseAngleScale},
        {"useAngleBrightness", g.UseAngleBrightness},
        {"useAngleCurve", g.UseAngleCurve},
        {"angleCurve", CurveToJson(g.AngleCurveKeys)},
        {"offScreenFadeDist", g.OffScreenFadeDist},
        {"useDynamicEdgeBoost", g.UseDynamicEdgeBoost},
        {"dynamicEdgeBrightness", g.DynamicEdgeBrightness},
        {"dynamicEdgeRange", g.DynamicEdgeRange},
        {"dynamicEdgeBias", g.DynamicEdgeBias},
        {"dynamicEdgeCurve", CurveToJson(g.DynamicEdgeCurveKeys)},
        {"useDynamicEdgeScale", g.UseDynamicEdgeScale},
        {"dynamicEdgeScale", g.DynamicEdgeScale},
        {"dynamicEdgeScaleRange", g.DynamicEdgeScaleRange},
        {"dynamicEdgeScaleBias", g.DynamicEdgeScaleBias},
        {"useDynamicCenterBoost", g.UseDynamicCenterBoost},
        {"dynamicCenterBrightness", g.DynamicCenterBrightness},
        {"dynamicCenterScale", g.DynamicCenterScale},
        {"dynamicCenterRange", g.DynamicCenterRange},
        {"dynamicCenterBias", g.DynamicCenterBias},
        {"multiplyScaleByTransformScale", g.MultiplyScaleByTransformScale},
        {"neverCull", g.NeverCull},
    };

    nlohmann::json elements = nlohmann::json::array();
    for (const LensFlare::FlareElement& e : m_Elements)
    {
        nlohmann::json element{
            {"sprite", e.SpriteName},
            {"visible", e.Visible},
            {"brightness", e.Brightness},
            {"scale", e.Scale},
            {"sizeX", e.SizeX},
            {"sizeY", e.SizeY},
            {"position", e.Position},
            {"offsetX", e.OffsetX},
            {"offsetY", e.OffsetY},
            {"anamorphicX", e.AnamorphicX},
            {"anamorphicY", e.AnamorphicY},
            {"angle", e.Angle},
            {"useStarRotation", e.UseStarRotation},
            {"rotateToFlare", e.RotateToFlare},
            {"rotationSpeed", e.RotationSpeed},
            {"tint", ColorToJson(e.Tint)},
        };
        if (e.EdgeBrightnessBoost != LensFlare::kInheritGlobalBoost)
            element["edgeBrightnessBoost"] = e.EdgeBrightnessBoost;
        if (e.CenterBrightnessBoost != LensFlare::kInheritGlobalBoost)
            element["centerBrightnessBoost"] = e.CenterBrightnessBoost;
        if (e.EdgeScaleBoost != LensFlare::kInheritGlobalBoost)
            element["edgeScaleBoost"] = e.EdgeScaleBoost;
        if (e.CenterScaleBoost != LensFlare::kInheritGlobalBoost)
            element["centerScaleBoost"] = e.CenterScaleBoost;
        elements.push_back(std::move(element));
    }

    const nlohmann::json document{
        {"atlas", m_AtlasRef},
        {"globals", std::move(globals)},
        {"elements", std::move(elements)},
    };

    std::ofstream out(GetPath(), std::ios::binary | std::ios::trunc);
    if (!out.is_open())
        return false;
    out << document.dump(2) << '\n';
    return out.good();
}

bool LensFlareDefinitionAsset::Load()
{
    String text;
    if (!ReadFileTextShared(GetPath(), text))
    {
        SetState(AssetState::Failed);
        return false;
    }
    const bool ok = ParseFromText(text);
    SetState(ok ? AssetState::Loaded : AssetState::Failed);
    return ok;
}

bool LensFlareDefinitionAsset::LoadFromData(const Vector<uint8>& data)
{
    const std::string text(reinterpret_cast<const char*>(data.data()), data.size());
    const bool ok = ParseFromText(text);
    SetState(ok ? AssetState::Loaded : AssetState::Failed);
    return ok;
}

void LensFlareDefinitionAsset::Unload()
{
    m_AtlasRef.clear();
    m_Globals = {};
    m_Elements.clear();
    SetState(AssetState::Unloaded);
}

bool LensFlareDefinitionAsset::Refuse(std::string_view reason)
{
    Logger::Log::Error("LensFlareDefinitionAsset: '{}' is not loaded: {}", GetPath().generic_string(), reason);
    m_AtlasRef.clear();
    m_Globals = {};
    m_Elements.clear();
    return false;
}

bool LensFlareDefinitionAsset::ParseFromText(const std::string& text)
{
    m_AtlasRef.clear();
    m_Globals = {};
    m_Elements.clear();

    nlohmann::json doc;
    std::string parseError;
    if (!Serialization::ParseJson(text, doc, parseError))
        return Refuse(std::format("it is not JSON ({}); fix the text at that place", parseError));
    if (!doc.is_object())
        return Refuse("its JSON is not an object; write it as an object of atlas, globals and elements");

    JsonFieldReader reader;
    reader.Read(doc, "atlas", m_AtlasRef);

    if (doc.contains("globals") && doc["globals"].is_object())
        ParseGlobals(doc["globals"], m_Globals, reader);

    if (doc.contains("elements") && doc["elements"].is_array())
    {
        const nlohmann::json& elements = doc["elements"];
        for (size_t index = 0; index < elements.size(); ++index)
        {
            if (elements[index].is_object())
                m_Elements.push_back(ParseElement(elements[index], index, reader));
        }
    }

    if (reader.Refused())
        return Refuse(reader.Refusal());
    return true;
}

} // namespace GameEngine
