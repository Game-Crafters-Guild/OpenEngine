#include "Assets/DayCycleProfile.h"

#include "AssetCore/SharedFileRead.h"
#include "Logger/Logger.h"
#include "Rendering/Sky/SkySunState.h"
#include "Scene/SceneValue.h"
#include "Types/ParseNumber.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>
#include <vector>

namespace GameEngine
{
namespace
{
constexpr std::string_view kNameField = "name";
constexpr std::string_view kDescriptionField = "description";
constexpr std::string_view kTracksField = "tracks";
constexpr std::string_view kKeysField = "keys";
constexpr std::string_view kSeparateDuskField = "separateDusk";
constexpr std::string_view kDuskKeysField = "duskKeys";
constexpr float kDegreesPerRadian = 180.0f / std::numbers::pi_v<float>;

std::string DeclaredTrackList()
{
    std::string list;
    for (const DayCycleTrackDesc& desc : kDayCycleTracks)
    {
        if (!list.empty())
            list += ", ";
        list += desc.FileKey;
    }
    return list;
}

// The fields of a key, in the order a key list writes them.
constexpr std::string_view kKeyFields[] = {"elevation", "value", "inTangent", "outTangent", "interpolation",
                                           "tangentMode"};
constexpr size_t kRequiredKeyFields = 2;
constexpr float kLowestElevationDegrees = -90.0f;
constexpr float kHighestElevationDegrees = 90.0f;
constexpr int kInterpolationCount = 3;
constexpr int kTangentModeCount = 5;
constexpr std::string_view kKeyFormat = "elevation,value,inTangent,outTangent,interpolation,tangentMode";

std::string_view TrimSpaces(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return text;
}

// The whole of `token` as one finite decimal number, or false: ParseStrictDecimal (no hex, inf, nan
// or trailing text, the same on every platform), without the leading '+' it allows, and finite as a
// float too.
bool ParseFiniteNumber(std::string_view token, float& out)
{
    token = TrimSpaces(token);
    double value = 0.0;
    if (token.empty() || token.front() == '+' || !ParseStrictDecimal(token, value))
        return false;
    const float narrowed = static_cast<float>(value);
    if (!std::isfinite(narrowed))
        return false;
    out = narrowed;
    return true;
}

std::vector<std::string_view> SplitOn(std::string_view text, char separator)
{
    std::vector<std::string_view> parts;
    size_t start = 0;
    while (true)
    {
        const size_t stop = text.find(separator, start);
        parts.push_back(text.substr(start, stop == std::string_view::npos ? text.size() - start : stop - start));
        if (stop == std::string_view::npos)
            return parts;
        start = stop + 1;
    }
}

// Reads one key (elevation,value[,inTangent,outTangent,interpolation,tangentMode]); `name` is
// "<where>: key <n>" for the refusal.
bool ParseKey(std::string_view text, const std::string& name, Math::CurveKey& out, std::string& outError)
{
    const std::vector<std::string_view> fields = SplitOn(text, ',');
    if (fields.size() < kRequiredKeyFields || fields.size() > std::size(kKeyFields))
    {
        outError = name + " has " + std::to_string(fields.size()) + " fields; write it as " + std::string(kKeyFormat) +
                   ", the last four optional";
        return false;
    }
    float values[std::size(kKeyFields)] = {};
    for (size_t i = 0; i < fields.size(); ++i)
    {
        if (!ParseFiniteNumber(fields[i], values[i]))
        {
            outError = name + "'s " + std::string(kKeyFields[i]) + " \"" + std::string(TrimSpaces(fields[i])) +
                       "\" is not a finite number; write a decimal number such as -6 or 1.5";
            return false;
        }
    }
    if (values[0] < kLowestElevationDegrees || values[0] > kHighestElevationDegrees)
    {
        outError = name + "'s elevation " + Scene::FormatFloat(values[0]) +
                   " is outside -90 to 90 degrees; write the sun's elevation above the horizon";
        return false;
    }
    const float interpolation = values[4];
    if (interpolation != std::floor(interpolation) || interpolation < 0.0f || interpolation >= kInterpolationCount)
    {
        outError = name + "'s interpolation " + Scene::FormatFloat(interpolation) +
                   " is not 0 (linear), 1 (smooth) or 2 (step); write one of them";
        return false;
    }
    const float tangentMode = values[5];
    if (tangentMode != std::floor(tangentMode) || tangentMode < 0.0f || tangentMode >= kTangentModeCount)
    {
        outError = name + "'s tangentMode " + Scene::FormatFloat(tangentMode) +
                   " is not 0 (auto), 1 (flat), 2 (broken), 3 (manual) or 4 (mirrored); write one of them";
        return false;
    }
    out.Time = values[0];
    out.Value = values[1];
    out.InTangent = values[2];
    out.OutTangent = values[3];
    out.Interp = static_cast<Math::CurveInterp>(static_cast<int>(interpolation));
    out.TangentMode = static_cast<Math::CurveTangentMode>(static_cast<int>(tangentMode));
    return true;
}

// Reads one key list (keys separated by ';', in ascending elevation) into `out`, each field once.
bool ParseKeyList(std::string_view text, const std::string& where, Math::Curve& out, std::string& outError)
{
    Math::Curve curve{};
    size_t count = 0;
    for (const std::string_view key : SplitOn(text, ';'))
    {
        if (TrimSpaces(key).empty())
            continue;
        ++count;
        if (count > Math::Curve::Capacity)
        {
            outError = where + ": more than the " + std::to_string(Math::Curve::Capacity) +
                       " keys a track holds; remove keys until " + std::to_string(Math::Curve::Capacity) + " remain";
            return false;
        }
        Math::CurveKey parsed{};
        if (!ParseKey(key, where + ": key " + std::to_string(count), parsed, outError))
            return false;
        if (curve.KeyCount > 0 && parsed.Time <= curve.Keys[curve.KeyCount - 1].Time)
        {
            outError = where + ": the keys are not in ascending elevation (" + Scene::FormatFloat(parsed.Time) +
                       " follows " + Scene::FormatFloat(curve.Keys[curve.KeyCount - 1].Time) +
                       "); write them from the lowest elevation up";
            return false;
        }
        curve.TryInsert(parsed);
    }
    if (count == 0)
    {
        outError = where + ": the track has no keys; write at least one key as " + std::string(kKeyFormat);
        return false;
    }
    out = curve;
    return true;
}

// The table row whose FileKey is `fileKey`, or null when the table declares no such track.
const DayCycleTrackDesc* FindTrackByFileKey(std::string_view fileKey)
{
    for (const DayCycleTrackDesc& desc : kDayCycleTracks)
    {
        if (desc.FileKey == fileKey)
            return &desc;
    }
    return nullptr;
}

// The first key written twice in one object of `text`, or empty. JSON leaves a repeated key to the
// reader, which keeps the last; a profile refuses it instead, so no definition is lost unseen.
std::string FirstRepeatedKey(std::string_view text)
{
    std::vector<std::vector<std::string>> objects;
    std::string repeated;
    const auto callback = [&](int, nlohmann::json::parse_event_t event, nlohmann::json& parsed) {
        if (event == nlohmann::json::parse_event_t::object_start)
            objects.emplace_back();
        else if (event == nlohmann::json::parse_event_t::object_end && !objects.empty())
            objects.pop_back();
        else if (event == nlohmann::json::parse_event_t::key && !objects.empty() && repeated.empty())
        {
            const std::string key = parsed.get<std::string>();
            std::vector<std::string>& seen = objects.back();
            if (std::find(seen.begin(), seen.end(), key) != seen.end())
                repeated = key;
            seen.push_back(key);
        }
        return true;
    };
    (void)nlohmann::json::parse(text, callback, false);
    return repeated;
}

bool ParseTrack(const DayCycleTrackDesc& desc, const nlohmann::json& value, DayCycleTrackKeys& out,
                std::string& outError)
{
    const std::string where = "track '" + std::string(desc.FileKey) + "'";
    if (!value.is_object())
    {
        outError = where + " is not an object; write it as { \"keys\": \"...\" }";
        return false;
    }
    for (const auto& [field, fieldValue] : value.items())
    {
        if (field != kKeysField && field != kSeparateDuskField && field != kDuskKeysField)
        {
            outError = where + " has the field '" + field +
                       "', which a track does not have; a track holds keys, separateDusk and duskKeys";
            return false;
        }
    }
    out.Track = desc.Track;
    const auto keys = value.find(kKeysField);
    if (keys == value.end() || !keys->is_string())
    {
        outError = where + " has no \"keys\" string; write \"keys\": \"" + std::string(kKeyFormat) + ";...\"";
        return false;
    }
    if (!ParseKeyList(keys->get<std::string>(), where, out.Keys, outError))
        return false;

    const auto separate = value.find(kSeparateDuskField);
    if (separate != value.end())
    {
        if (!separate->is_boolean())
        {
            outError = where + ": separateDusk is not true or false; write true or false";
            return false;
        }
        out.SeparateDusk = separate->get<bool>();
    }
    const auto dusk = value.find(kDuskKeysField);
    if (out.SeparateDusk != (dusk != value.end()))
    {
        outError = out.SeparateDusk ? where + " separates the dusk and has no \"duskKeys\""
                                    : where + " has \"duskKeys\" without \"separateDusk\": true";
        return false;
    }
    if (!out.SeparateDusk)
        return true;
    if (!dusk->is_string())
    {
        outError = where + ": duskKeys is not a string; write the dusk keys as one string, as keys is";
        return false;
    }
    return ParseKeyList(dusk->get<std::string>(), where + " (dusk)", out.DuskKeys, outError);
}

bool ReadString(const nlohmann::json& root, std::string_view field, std::string& out, std::string& outError)
{
    const auto it = root.find(field);
    if (it == root.end())
        return true;
    if (!it->is_string())
    {
        outError = "\"" + std::string(field) + "\" is not a string; write it in quotes";
        return false;
    }
    out = it->get<std::string>();
    return true;
}
} // namespace

const DayCycleTrackKeys* DayCycleProfileData::Find(DayCycleTrack track) const
{
    for (const DayCycleTrackKeys& keys : Tracks)
    {
        if (keys.Track == track)
            return &keys;
    }
    return nullptr;
}

DayCycleTrackKeys* DayCycleProfileData::Find(DayCycleTrack track)
{
    return const_cast<DayCycleTrackKeys*>(std::as_const(*this).Find(track));
}

bool ParseDayCycleProfile(std::string_view text, DayCycleProfileData& out, std::string& outError)
{
    const nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object())
    {
        outError = "the file is not a JSON object";
        return false;
    }
    if (const std::string repeated = FirstRepeatedKey(text); !repeated.empty())
    {
        outError = "'" + repeated + "' is written twice; keep one";
        return false;
    }
    for (const auto& [field, value] : root.items())
    {
        if (field != kNameField && field != kDescriptionField && field != kTracksField)
        {
            outError = "the field '" + field +
                       "' is not part of the Day Cycle profile format, which holds name, description and tracks; "
                       "remove it";
            return false;
        }
    }

    DayCycleProfileData data;
    if (!ReadString(root, kNameField, data.Name, outError) ||
        !ReadString(root, kDescriptionField, data.Description, outError))
        return false;

    const auto tracks = root.find(kTracksField);
    if (tracks != root.end())
    {
        if (!tracks->is_object())
        {
            outError = "\"tracks\" is not an object; write it as { \"exposureOffset\": { \"keys\": \"...\" } }";
            return false;
        }
        for (const auto& [name, value] : tracks->items())
        {
            if (!FindTrackByFileKey(name))
            {
                outError = "'" + name + "' is not a Day Cycle track; the tracks are: " + DeclaredTrackList() +
                           ". Rename the track or remove it";
                return false;
            }
        }
        // Table order, whatever order the file wrote them in.
        for (const DayCycleTrackDesc& desc : kDayCycleTracks)
        {
            const auto it = tracks->find(desc.FileKey);
            if (it == tracks->end())
                continue;
            DayCycleTrackKeys keys;
            if (!ParseTrack(desc, *it, keys, outError))
                return false;
            data.Tracks.push_back(keys);
        }
    }
    out = std::move(data);
    return true;
}

std::string WriteDayCycleProfile(const DayCycleProfileData& data)
{
    nlohmann::ordered_json root;
    root[std::string(kNameField)] = data.Name;
    root[std::string(kDescriptionField)] = data.Description;
    nlohmann::ordered_json tracks = nlohmann::ordered_json::object();
    for (const DayCycleTrackKeys& keys : data.Tracks)
    {
        nlohmann::ordered_json track;
        track[std::string(kKeysField)] = Scene::FormatCurveKeys(keys.Keys);
        if (keys.SeparateDusk)
        {
            track[std::string(kSeparateDuskField)] = true;
            track[std::string(kDuskKeysField)] = Scene::FormatCurveKeys(keys.DuskKeys);
        }
        tracks[std::string(GetDayCycleTrackDesc(keys.Track).FileKey)] = std::move(track);
    }
    root[std::string(kTracksField)] = std::move(tracks);
    return root.dump(2) + "\n";
}

float EvaluateDayCycleTrack(const DayCycleTrackKeys& keys, Mathematics::Radians elevation, bool duskHalf)
{
    const Math::Curve& curve = (keys.SeparateDusk && duskHalf) ? keys.DuskKeys : keys.Keys;
    return curve.Evaluate(elevation * kDegreesPerRadian);
}

bool IsDuskHalf(const Rendering::SkySunState& sunState)
{
    return sunState.SunAzimuth > std::numbers::pi_v<float>;
}

DayCycleProfile::DayCycleProfile(const GUID& guid, const std::filesystem::path& path)
    : Asset(guid, AssetType::DayCycleProfile, path)
{
}

bool DayCycleProfile::Load()
{
    Vector<uint8> bytes;
    if (!ReadFileBytesShared(GetPath(), bytes))
    {
        Logger::Log::Error("Day Cycle profile '{}' could not be read", GetPath().string());
        SetState(AssetState::Failed);
        return false;
    }
    return LoadFromData(bytes);
}

bool DayCycleProfile::LoadFromData(const Vector<uint8>& data)
{
    std::shared_ptr<const DayCycleProfileData> parsed = ParseBytes(data);
    if (!parsed)
    {
        SetState(AssetState::Failed);
        return false;
    }
    Publish(std::move(parsed));
    SetState(AssetState::Loaded);
    return true;
}

void DayCycleProfile::Unload()
{
    Publish(nullptr);
    SetState(AssetState::Unloaded);
}

bool DayCycleProfile::ReloadFromData(const Vector<uint8>& data)
{
    std::shared_ptr<const DayCycleProfileData> parsed = ParseBytes(data);
    if (!parsed)
        return false;
    Publish(std::move(parsed));
    return true;
}

std::shared_ptr<const DayCycleProfileData> DayCycleProfile::GetData() const
{
    std::lock_guard lock(m_DataMutex);
    return m_Data;
}

std::shared_ptr<const DayCycleProfileData> DayCycleProfile::ParseBytes(const Vector<uint8>& data) const
{
    auto parsed = std::make_shared<DayCycleProfileData>();
    std::string error;
    if (!ParseDayCycleProfile(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()), *parsed,
                              error))
    {
        Logger::Log::Error("Day Cycle profile '{}' was not loaded: {}", GetPath().string(), error);
        return nullptr;
    }
    return parsed;
}

void DayCycleProfile::Publish(std::shared_ptr<const DayCycleProfileData> data)
{
    {
        std::lock_guard lock(m_DataMutex);
        m_Data.swap(data);
    }
    // `data` holds the previous profile now and is released here, outside the lock.
}

} // namespace GameEngine
