#include "Animation/SkeletonProfile.h"

#include "AssetCore/SharedFileRead.h"
#include "Logger/Logger.h"
#include "Serialization/JsonRead.h"

#include <format>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <string_view>

namespace GameEngine
{
namespace Animation
{

namespace
{

using json = nlohmann::json;

json Vec3ToJson(const Mathematics::Vector3& v)
{
    return json::array({v.x, v.y, v.z});
}

json QuatToJson(const Mathematics::Quaternion& q)
{
    const glm::quat& g = q.GetGLM();
    return json::array({g.w, g.x, g.y, g.z});
}

} // namespace

bool SkeletonProfile::Load()
{
    SetState(AssetState::Loading);

    String text;
    if (!ReadFileTextShared(GetPath(), text))
    {
        Logger::Log::Error("SkeletonProfile: cannot open '{}'", GetPath().string());
        SetState(AssetState::Failed);
        return false;
    }
    return ParseJson(text);
}

bool SkeletonProfile::LoadFromData(const Vector<uint8>& data)
{
    SetState(AssetState::Loading);
    const std::string text(reinterpret_cast<const char*>(data.data()), data.size());
    return ParseJson(text);
}

void SkeletonProfile::Unload()
{
    m_Name.clear();
    m_Description.clear();
    m_Version = 0;
    m_Bones.clear();
    SetState(AssetState::Unloaded);
}

bool SkeletonProfile::SaveToData(Vector<uint8>& outData) const
{
    const std::string serialized = SerializeJson();
    outData.assign(serialized.begin(), serialized.end());
    return true;
}

bool SkeletonProfile::SaveToPath(const std::filesystem::path& path) const
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open())
    {
        Logger::Log::Error("SkeletonProfile: cannot open '{}' for write", path.string());
        return false;
    }
    const std::string serialized = SerializeJson();
    file.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
    return file.good();
}

const ProfileBone* SkeletonProfile::FindBone(HumanBone bone) const
{
    for (const auto& pb : m_Bones)
    {
        if (pb.Bone == bone)
            return &pb;
    }
    return nullptr;
}

bool SkeletonProfile::ParseJson(const std::string& text)
{
    json doc;
    std::string parseError;
    if (!Serialization::ParseJson(text, doc, parseError))
        return Refuse(std::format("it is not JSON ({}); fix the text at that place", parseError));

    Serialization::JsonFieldReader reader;
    // Schema version. Missing = 0; bumped (newer than supported) logs a warning
    // but proceeds with best-effort parsing for forward compatibility.
    m_Version = 0;
    reader.Read(doc, "version", m_Version);
    if (m_Version > kSchemaVersion)
    {
        Logger::Log::Warning(
            "SkeletonProfile '{}' has version {} but engine supports up to {}; reading anyway",
            GetPath().string(), m_Version, kSchemaVersion);
    }

    m_Name.clear();
    m_Description.clear();
    reader.Read(doc, "name", m_Name);
    reader.Read(doc, "description", m_Description);

    m_Bones.clear();
    if (doc.contains("bones") && doc["bones"].is_array())
    {
        m_Bones.reserve(doc["bones"].size());
        const json& bones = doc["bones"];
        for (size_t index = 0; index < bones.size(); ++index)
        {
            const json& boneJson = bones[index];
            const std::string context = std::format("bones[{}].", index);
            reader.SetContext(context);
            ProfileBone pb;
            std::string bone = "None";
            std::string parent = "None";
            reader.Read(boneJson, "bone", bone);
            reader.Read(boneJson, "parent", parent);
            pb.Bone = HumanBoneFromString(bone);
            pb.Parent = HumanBoneFromString(parent);
            pb.RestTranslation =
                reader.ReadVector3(boneJson, "restTranslation", Mathematics::Vector3(0.0f, 0.0f, 0.0f));
            pb.RestRotation = reader.ReadQuaternion(boneJson, "restRotation", Mathematics::Quaternion::Identity());
            pb.RestScale = reader.ReadVector3(boneJson, "restScale", Mathematics::Vector3(1.0f, 1.0f, 1.0f));
            pb.LocalForward = reader.ReadVector3(boneJson, "localForward", Mathematics::Vector3(0.0f, 0.0f, 1.0f));
            pb.LocalUp = reader.ReadVector3(boneJson, "localUp", Mathematics::Vector3(0.0f, 1.0f, 0.0f));
            m_Bones.push_back(pb);
        }
    }
    if (reader.Refused())
        return Refuse(reader.Refusal());

    SetState(AssetState::Loaded);
    return true;
}

bool SkeletonProfile::Refuse(std::string_view reason)
{
    Logger::Log::Error("SkeletonProfile: '{}' is not loaded: {}", GetPath().generic_string(), reason);
    Unload();
    SetState(AssetState::Failed);
    return false;
}

std::string SkeletonProfile::SerializeJson() const
{
    json doc;
    doc["version"] = kSchemaVersion;
    doc["name"] = m_Name;
    doc["description"] = m_Description;

    json bones = json::array();
    for (const auto& pb : m_Bones)
    {
        json b;
        b["bone"] = HumanBoneToString(pb.Bone);
        b["parent"] = HumanBoneToString(pb.Parent);
        b["restTranslation"] = Vec3ToJson(pb.RestTranslation);
        b["restRotation"] = QuatToJson(pb.RestRotation);
        b["restScale"] = Vec3ToJson(pb.RestScale);
        b["localForward"] = Vec3ToJson(pb.LocalForward);
        b["localUp"] = Vec3ToJson(pb.LocalUp);
        bones.push_back(std::move(b));
    }
    doc["bones"] = std::move(bones);

    return doc.dump(2);
}

} // namespace Animation
} // namespace GameEngine
