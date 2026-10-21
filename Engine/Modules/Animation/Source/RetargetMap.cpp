#include "Animation/RetargetMap.h"

#include "AssetCore/SharedFileRead.h"
#include "Logger/Logger.h"
#include "Serialization/JsonRead.h"

#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace GameEngine
{
namespace Animation
{

namespace
{

using json = nlohmann::json;

constexpr const char* kFKRotationModeNames[] = {
    "OneToOne",
    "Transport",
    "SlerpAlongArc"
};

constexpr const char* kFKTranslationModeNames[] = {
    "None",
    "PerBoneScale",
    "UniformChainScale"
};

} // namespace

const char* FKRotationModeToString(FKRotationMode mode)
{
    const auto idx = static_cast<size_t>(mode);
    if (idx >= sizeof(kFKRotationModeNames) / sizeof(kFKRotationModeNames[0]))
        return "OneToOne";
    return kFKRotationModeNames[idx];
}

FKRotationMode FKRotationModeFromString(std::string_view name)
{
    for (size_t i = 0; i < sizeof(kFKRotationModeNames) / sizeof(kFKRotationModeNames[0]); ++i)
    {
        if (name == kFKRotationModeNames[i])
            return static_cast<FKRotationMode>(i);
    }
    return FKRotationMode::OneToOne;
}

const char* FKTranslationModeToString(FKTranslationMode mode)
{
    const auto idx = static_cast<size_t>(mode);
    if (idx >= sizeof(kFKTranslationModeNames) / sizeof(kFKTranslationModeNames[0]))
        return "None";
    return kFKTranslationModeNames[idx];
}

FKTranslationMode FKTranslationModeFromString(std::string_view name)
{
    for (size_t i = 0; i < sizeof(kFKTranslationModeNames) / sizeof(kFKTranslationModeNames[0]); ++i)
    {
        if (name == kFKTranslationModeNames[i])
            return static_cast<FKTranslationMode>(i);
    }
    return FKTranslationMode::None;
}

bool RetargetMap::Load()
{
    SetState(AssetState::Loading);

    String text;
    if (!ReadFileTextShared(GetPath(), text))
    {
        Logger::Log::Error("RetargetMap: cannot open '{}'", GetPath().string());
        SetState(AssetState::Failed);
        return false;
    }
    return ParseJson(text);
}

bool RetargetMap::LoadFromData(const Vector<uint8>& data)
{
    SetState(AssetState::Loading);
    const std::string text(reinterpret_cast<const char*>(data.data()), data.size());
    return ParseJson(text);
}

void RetargetMap::Unload()
{
    m_Version = 0;
    m_SourceRigRef = ::GameEngine::GUID();
    m_TargetRigRef = ::GameEngine::GUID();
    m_ChainMap.clear();
    m_OpStack.clear();
    SetState(AssetState::Unloaded);
}

bool RetargetMap::SaveToData(Vector<uint8>& outData) const
{
    const std::string serialized = SerializeJson();
    outData.assign(serialized.begin(), serialized.end());
    return true;
}

bool RetargetMap::SaveToPath(const std::filesystem::path& path) const
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open())
    {
        Logger::Log::Error("RetargetMap: cannot open '{}' for write", path.string());
        return false;
    }
    const std::string serialized = SerializeJson();
    file.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
    return file.good();
}

bool RetargetMap::ParseJson(const std::string& text)
{
    json doc;
    std::string parseError;
    if (!Serialization::ParseJson(text, doc, parseError))
        return Refuse(std::format("it is not JSON ({}); fix the text at that place", parseError));

    Serialization::JsonFieldReader reader;
    m_Version = 0;
    reader.Read(doc, "version", m_Version);
    if (m_Version > kSchemaVersion)
    {
        Logger::Log::Warning(
            "RetargetMap '{}' has version {} but engine supports up to {}; reading anyway",
            GetPath().string(), m_Version, kSchemaVersion);
    }

    if (doc.contains("sourceRigRef") && doc["sourceRigRef"].is_string())
        m_SourceRigRef = ::GameEngine::GUID(doc["sourceRigRef"].get<std::string>());
    else
        m_SourceRigRef = ::GameEngine::GUID();

    if (doc.contains("targetRigRef") && doc["targetRigRef"].is_string())
        m_TargetRigRef = ::GameEngine::GUID(doc["targetRigRef"].get<std::string>());
    else
        m_TargetRigRef = ::GameEngine::GUID();

    m_ChainMap.clear();
    if (doc.contains("chainMap") && doc["chainMap"].is_array())
    {
        m_ChainMap.reserve(doc["chainMap"].size());
        const json& chainMap = doc["chainMap"];
        for (size_t index = 0; index < chainMap.size(); ++index)
        {
            const json& c = chainMap[index];
            const std::string context = std::format("chainMap[{}].", index);
            reader.SetContext(context);
            ChainPairing cp;
            std::string kind = "Other";
            reader.Read(c, "kind", kind);
            cp.Kind = ChainKindFromString(kind);

            if (c.is_object() && c.contains("fk") && c["fk"].is_object())
            {
                const auto& fk = c["fk"];
                reader.SetContext(context + "fk.");
                std::string rotationMode = "OneToOne";
                std::string translationMode = "None";
                reader.Read(fk, "rotationMode", rotationMode);
                reader.Read(fk, "rotationAlpha", cp.FK.RotationAlpha);
                reader.Read(fk, "translationMode", translationMode);
                cp.FK.RotationMode = FKRotationModeFromString(rotationMode);
                cp.FK.TranslationMode = FKTranslationModeFromString(translationMode);
            }

            if (c.is_object() && c.contains("ik") && c["ik"].is_object())
            {
                const auto& ik = c["ik"];
                reader.SetContext(context + "ik.");
                reader.Read(ik, "enabled", cp.IK.Enabled);
                reader.Read(ik, "blendToSource", cp.IK.BlendToSource);
                reader.Read(ik, "extension", cp.IK.Extension);
            }

            if (c.is_object() && c.contains("footLock") && c["footLock"].is_object())
            {
                const auto& fl = c["footLock"];
                reader.SetContext(context + "footLock.");
                reader.Read(fl, "enabled", cp.FootLock.Enabled);
                reader.Read(fl, "speedThreshold", cp.FootLock.SpeedThreshold);
                reader.Read(fl, "lockBlend", cp.FootLock.LockBlend);
            }

            m_ChainMap.push_back(std::move(cp));
        }
    }

    m_OpStack.clear();
    if (doc.contains("opStack") && doc["opStack"].is_array())
    {
        m_OpStack.reserve(doc["opStack"].size());
        const json& opStack = doc["opStack"];
        for (size_t index = 0; index < opStack.size(); ++index)
        {
            const json& op = opStack[index];
            reader.SetContext(std::format("opStack[{}].", index));
            OpStackEntry ose;
            reader.Read(op, "opName", ose.OpName);
            if (op.is_object() && op.contains("params"))
                ose.Params = op["params"];
            m_OpStack.push_back(std::move(ose));
        }
    }
    if (reader.Refused())
        return Refuse(reader.Refusal());

    SetState(AssetState::Loaded);
    return true;
}

bool RetargetMap::Refuse(std::string_view reason)
{
    Logger::Log::Error("RetargetMap: '{}' is not loaded: {}", GetPath().generic_string(), reason);
    Unload();
    SetState(AssetState::Failed);
    return false;
}

std::string RetargetMap::SerializeJson() const
{
    json doc;
    doc["version"] = kSchemaVersion;
    doc["sourceRigRef"] = m_SourceRigRef.ToString();
    doc["targetRigRef"] = m_TargetRigRef.ToString();

    json cm = json::array();
    for (const auto& cp : m_ChainMap)
    {
        json e;
        e["kind"] = ChainKindToString(cp.Kind);

        json fk;
        fk["rotationMode"] = FKRotationModeToString(cp.FK.RotationMode);
        fk["rotationAlpha"] = cp.FK.RotationAlpha;
        fk["translationMode"] = FKTranslationModeToString(cp.FK.TranslationMode);
        e["fk"] = std::move(fk);

        json ik;
        ik["enabled"] = cp.IK.Enabled;
        ik["blendToSource"] = cp.IK.BlendToSource;
        ik["extension"] = cp.IK.Extension;
        e["ik"] = std::move(ik);

        json fl;
        fl["enabled"] = cp.FootLock.Enabled;
        fl["speedThreshold"] = cp.FootLock.SpeedThreshold;
        fl["lockBlend"] = cp.FootLock.LockBlend;
        e["footLock"] = std::move(fl);

        cm.push_back(std::move(e));
    }
    doc["chainMap"] = std::move(cm);

    json os = json::array();
    for (const auto& op : m_OpStack)
    {
        json e;
        e["opName"] = op.OpName;
        e["params"] = op.Params;
        os.push_back(std::move(e));
    }
    doc["opStack"] = std::move(os);

    return doc.dump(2);
}

} // namespace Animation
} // namespace GameEngine
