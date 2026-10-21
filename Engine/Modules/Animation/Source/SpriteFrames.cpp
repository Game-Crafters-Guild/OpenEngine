#include "Animation/SpriteFrames.h"

#include "AssetCore/SharedFileRead.h"
#include "Logger/Logger.h"
#include "Serialization/JsonRead.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <iterator>
#include <nlohmann/json.hpp>
#include <utility>

namespace GameEngine
{
namespace Animation
{

namespace
{
using json = nlohmann::json;

float32 AnimationDuration(const SpriteFrameAnimation& animation)
{
    float32 duration = 0.0f;
    for (const auto& frame : animation.Frames)
        duration += std::max(0.0f, frame.DurationSeconds);
    return duration;
}

} // namespace

bool SpriteFrames::Load()
{
    SetState(AssetState::Loading);
    String text;
    if (!ReadFileTextShared(GetPath(), text))
    {
        Logger::Log::Error("SpriteFrames: cannot open '{}'", GetPath().string());
        SetState(AssetState::Failed);
        return false;
    }
    return ParseJson(text);
}

bool SpriteFrames::LoadFromData(const Vector<uint8>& data)
{
    SetState(AssetState::Loading);
    const std::string text(reinterpret_cast<const char*>(data.data()), data.size());
    return ParseJson(text);
}

void SpriteFrames::Unload()
{
    m_Animations.clear();
    SetState(AssetState::Unloaded);
}

bool SpriteFrames::SaveToData(Vector<uint8>& outData) const
{
    const std::string serialized = SerializeJson();
    outData.assign(serialized.begin(), serialized.end());
    return true;
}

const SpriteFrameAnimation* SpriteFrames::FindAnimation(const std::string& name) const
{
    for (const auto& animation : m_Animations)
    {
        if (animation.Name == name)
            return &animation;
    }
    return nullptr;
}

const SpriteFrame* SpriteFrames::Sample(const std::string& animationName, float32 timeSeconds) const
{
    const auto* animation = FindAnimation(animationName);
    if (!animation || animation->Frames.empty())
        return nullptr;

    const float32 duration = AnimationDuration(*animation);
    if (duration <= 0.0f)
        return &animation->Frames.front();

    float32 t = timeSeconds;
    if (animation->Loop)
    {
        t = std::fmod(std::max(0.0f, t), duration);
    }
    else
    {
        t = std::max(0.0f, std::min(t, duration));
    }

    float32 cursor = 0.0f;
    for (const auto& frame : animation->Frames)
    {
        cursor += std::max(0.0f, frame.DurationSeconds);
        if (t <= cursor)
            return &frame;
    }
    return &animation->Frames.back();
}

bool SpriteFrames::ParseJson(const std::string& text)
{
    json doc;
    std::string parseError;
    if (!Serialization::ParseJson(text, doc, parseError))
        return Refuse(std::format("it is not JSON ({}); fix the text at that place", parseError));
    if (!doc.is_object())
        return Refuse("its JSON is not an object; write it as an object holding an \"animations\" array");

    m_Animations.clear();
    Serialization::JsonFieldReader reader;
    if (doc.contains("animations") && doc["animations"].is_array())
    {
        const json& animations = doc["animations"];
        for (size_t animationIndex = 0; animationIndex < animations.size(); ++animationIndex)
        {
            const json& animJson = animations[animationIndex];
            const std::string context = std::format("animations[{}].", animationIndex);
            reader.SetContext(context);
            SpriteFrameAnimation animation;
            reader.Read(animJson, "name", animation.Name);
            reader.Read(animJson, "loop", animation.Loop);
            if (animJson.is_object() && animJson.contains("frames") && animJson["frames"].is_array())
            {
                const json& frames = animJson["frames"];
                for (size_t frameIndex = 0; frameIndex < frames.size(); ++frameIndex)
                {
                    const json& frameJson = frames[frameIndex];
                    reader.SetContext(std::format("{}frames[{}].", context, frameIndex));
                    SpriteFrame frame;
                    reader.Read(frameJson, "name", frame.Name);
                    reader.Read(frameJson, "durationSeconds", frame.DurationSeconds);
                    frame.DurationSeconds = std::max(0.0f, frame.DurationSeconds);
                    if (frameJson.is_object() && frameJson.contains("textureGuid") && frameJson["textureGuid"].is_string())
                        frame.TextureGuid = GUID(frameJson["textureGuid"].get<std::string>());
                    reader.Read(frameJson, "u0", frame.U0);
                    reader.Read(frameJson, "v0", frame.V0);
                    reader.Read(frameJson, "u1", frame.U1);
                    reader.Read(frameJson, "v1", frame.V1);
                    animation.Frames.push_back(std::move(frame));
                }
            }
            if (!animation.Name.empty())
                m_Animations.push_back(std::move(animation));
        }
    }
    if (reader.Refused())
        return Refuse(reader.Refusal());

    SetState(AssetState::Loaded);
    return true;
}

bool SpriteFrames::Refuse(std::string_view reason)
{
    Logger::Log::Error("SpriteFrames: '{}' is not loaded: {}", GetPath().generic_string(), reason);
    m_Animations.clear();
    SetState(AssetState::Failed);
    return false;
}

std::string SpriteFrames::SerializeJson() const
{
    json doc;
    doc["schemaVersion"] = kSchemaVersion;
    doc["assetType"] = "SpriteFrames";
    json animations = json::array();
    for (const auto& animation : m_Animations)
    {
        json animJson;
        animJson["name"] = animation.Name;
        animJson["loop"] = animation.Loop;
        json frames = json::array();
        for (const auto& frame : animation.Frames)
        {
            frames.push_back(json{
                {"name", frame.Name},
                {"textureGuid", frame.TextureGuid.ToString()},
                {"durationSeconds", frame.DurationSeconds},
                {"u0", frame.U0},
                {"v0", frame.V0},
                {"u1", frame.U1},
                {"v1", frame.V1}
            });
        }
        animJson["frames"] = std::move(frames);
        animations.push_back(std::move(animJson));
    }
    doc["animations"] = std::move(animations);
    return doc.dump(2);
}

} // namespace Animation
} // namespace GameEngine
