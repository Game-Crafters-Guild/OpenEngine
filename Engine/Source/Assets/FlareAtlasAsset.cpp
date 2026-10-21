#include "Assets/FlareAtlasAsset.h"

#include "AssetCore/SharedFileRead.h"
#include "Logger/Logger.h"
#include "Serialization/JsonRead.h"

#include <format>

#include <nlohmann/json.hpp>

namespace GameEngine {

bool FlareAtlasAsset::Load()
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

bool FlareAtlasAsset::LoadFromData(const Vector<uint8>& data)
{
    const std::string text(reinterpret_cast<const char*>(data.data()), data.size());
    const bool ok = ParseFromText(text);
    SetState(ok ? AssetState::Loaded : AssetState::Failed);
    return ok;
}

void FlareAtlasAsset::Unload()
{
    m_TextureRef.clear();
    m_Sprites.clear();
    SetState(AssetState::Unloaded);
}

int32_t FlareAtlasAsset::FindSprite(const std::string& name) const
{
    for (size_t i = 0; i < m_Sprites.size(); ++i)
    {
        if (m_Sprites[i].Name == name)
            return static_cast<int32_t>(i);
    }
    return -1;
}

bool FlareAtlasAsset::Refuse(std::string_view reason)
{
    Logger::Log::Error("FlareAtlasAsset: '{}' is not loaded: {}", GetPath().generic_string(), reason);
    m_TextureRef.clear();
    m_Sprites.clear();
    return false;
}

bool FlareAtlasAsset::ParseFromText(const std::string& text)
{
    m_TextureRef.clear();
    m_Sprites.clear();

    nlohmann::json doc;
    std::string parseError;
    if (!Serialization::ParseJson(text, doc, parseError))
        return Refuse(std::format("it is not JSON ({}); fix the text at that place", parseError));
    if (!doc.is_object())
        return Refuse("its JSON is not an object; write it as an object of texture and sprites");

    Serialization::JsonFieldReader reader;
    reader.Read(doc, "texture", m_TextureRef);

    if (doc.contains("sprites") && doc["sprites"].is_array())
    {
        const nlohmann::json& sprites = doc["sprites"];
        for (size_t index = 0; index < sprites.size(); ++index)
        {
            const nlohmann::json& s = sprites[index];
            if (!s.is_object())
                continue;
            reader.SetContext(std::format("sprites[{}].", index));
            LensFlare::AtlasSprite sprite;
            reader.Read(s, "name", sprite.Name);
            reader.Read(s, "u", sprite.U);
            reader.Read(s, "v", sprite.V);
            reader.Read(s, "w", sprite.W);
            reader.Read(s, "h", sprite.H);
            m_Sprites.push_back(std::move(sprite));
        }
    }

    if (reader.Refused())
        return Refuse(reader.Refusal());
    return true;
}

} // namespace GameEngine
