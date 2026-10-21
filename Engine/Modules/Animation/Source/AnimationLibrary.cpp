#include "Animation/AnimationLibrary.h"

#include "AssetCore/AssetTypes.h"
#include "AssetCore/SharedFileRead.h"
#include "Logger/Logger.h"
#include "Serialization/JsonRead.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>

namespace GameEngine
{
namespace Animation
{

namespace
{

using json = nlohmann::json;

AssetType AnimationAssetTypeFromString(const std::string& value)
{
    if (value == "Timeline") return AssetType::Timeline;
    if (value == "ClipSet") return AssetType::ClipSet;
    if (value == "AnimationLibrary") return AssetType::AnimationLibrary;
    if (value == "AnimationController") return AssetType::AnimationController;
    if (value == "SpriteFrames") return AssetType::SpriteFrames;
    return AssetType::Animation;
}

} // namespace

bool AnimationLibrary::Load()
{
    SetState(AssetState::Loading);

    String text;
    if (!ReadFileTextShared(GetPath(), text))
    {
        Logger::Log::Error("AnimationLibrary: cannot open '{}'", GetPath().string());
        SetState(AssetState::Failed);
        return false;
    }
    return ParseJson(text);
}

bool AnimationLibrary::LoadFromData(const Vector<uint8>& data)
{
    SetState(AssetState::Loading);
    const std::string text(reinterpret_cast<const char*>(data.data()), data.size());
    return ParseJson(text);
}

void AnimationLibrary::Unload()
{
    m_Entries.clear();
    SetState(AssetState::Unloaded);
}

bool AnimationLibrary::SaveToData(Vector<uint8>& outData) const
{
    const std::string serialized = SerializeJson();
    outData.assign(serialized.begin(), serialized.end());
    return true;
}

bool AnimationLibrary::SaveToPath(const std::filesystem::path& path) const
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open())
    {
        Logger::Log::Error("AnimationLibrary: cannot open '{}' for write", path.string());
        return false;
    }
    const std::string serialized = SerializeJson();
    file.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
    return file.good();
}

const AnimationLibraryEntry* AnimationLibrary::Find(const std::string& name) const
{
    for (const auto& entry : m_Entries)
    {
        if (entry.Name == name)
            return &entry;
    }
    return nullptr;
}

bool AnimationLibrary::ParseJson(const std::string& text)
{
    json doc;
    std::string parseError;
    if (!Serialization::ParseJson(text, doc, parseError))
        return Refuse(std::format("it is not JSON ({}); fix the text at that place", parseError));
    if (!doc.is_object())
        return Refuse("its JSON is not an object; write it as an object holding an \"animations\" array");

    m_Entries.clear();
    Serialization::JsonFieldReader reader;
    if (doc.contains("animations") && doc["animations"].is_array())
    {
        const json& animations = doc["animations"];
        for (size_t index = 0; index < animations.size(); ++index)
        {
            const json& entryJson = animations[index];
            reader.SetContext(std::format("animations[{}].", index));
            AnimationLibraryEntry entry;
            reader.Read(entryJson, "name", entry.Name);
            if (entry.Name.empty())
                continue;

            if (entryJson.contains("assetGuid") && entryJson["assetGuid"].is_string())
                entry.AssetGuid = GUID(entryJson["assetGuid"].get<std::string>());

            std::string type = "Animation";
            reader.Read(entryJson, "type", type);
            entry.Type = AnimationAssetTypeFromString(type);
            float defaultBlendSeconds = 0.0f;
            reader.Read(entryJson, "defaultBlendSeconds", defaultBlendSeconds);
            entry.DefaultBlendSeconds = std::max(0.0f, defaultBlendSeconds);
            reader.Read(entryJson, "loop", entry.Loop);
            m_Entries.push_back(std::move(entry));
        }
    }
    if (reader.Refused())
        return Refuse(reader.Refusal());

    SetState(AssetState::Loaded);
    return true;
}

bool AnimationLibrary::Refuse(std::string_view reason)
{
    Logger::Log::Error("AnimationLibrary: '{}' is not loaded: {}", GetPath().generic_string(), reason);
    m_Entries.clear();
    SetState(AssetState::Failed);
    return false;
}

std::string AnimationLibrary::SerializeJson() const
{
    json doc;
    doc["schemaVersion"] = kSchemaVersion;
    doc["assetType"] = "AnimationLibrary";

    json animations = json::array();
    for (const auto& entry : m_Entries)
    {
        json item;
        item["name"] = entry.Name;
        item["assetGuid"] = entry.AssetGuid.ToString();
        item["type"] = AssetTypeToString(entry.Type);
        item["defaultBlendSeconds"] = entry.DefaultBlendSeconds;
        item["loop"] = entry.Loop;
        animations.push_back(std::move(item));
    }
    doc["animations"] = std::move(animations);
    return doc.dump(2);
}

} // namespace Animation
} // namespace GameEngine
