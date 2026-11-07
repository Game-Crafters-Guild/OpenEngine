#include "HistoryValueFormatter.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/MaterialAsset.h"
#include "Components/Name.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/Component/ComponentFieldRegistry.h"
#include "ECS/ECSTemplates.h"
#include "ECS/ReflectionJson.h"
#include "Types/FormatNumber.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace GameEngine::Editor
{
namespace
{
using Json = nlohmann::ordered_json;

void AppendChange(std::string& result, std::string_view label, const std::string& before, const std::string& after,
                  bool valuesDiffer = false)
{
    if (before == after && !valuesDiffer)
        return;
    if (!result.empty())
        result += '\n';
    result += std::format("{}: {} -> {}", label, before, after);
}

std::string FormatReference(const std::string& value)
{
    const GUID guid(value);
    if (!guid.IsNull())
    {
        AssetManager* assets = EngineCore::GetInstance().TryGetAssetManager();
        AssetMetadata metadata;
        if (assets && assets->GetRegistry().TryGetAssetMetadata(guid, metadata))
        {
            const std::string name = metadata.Name.empty() ? metadata.Path.stem().string() : metadata.Name;
            if (!name.empty())
                return Json(name).dump(-1, ' ', false, Json::error_handler_t::replace);
        }
    }
    return Json(value).dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::string FormatValue(const Json& value, bool singlePrecision = false)
{
    if (value.is_string())
        return FormatReference(value.get<std::string>());
    if (value.is_number_float())
        return singlePrecision ? FormatFloat(value.get<float>()) : FormatDouble(value.get<double>());
    if (!value.is_array())
        return value.dump(-1, ' ', false, Json::error_handler_t::replace);
    std::vector<std::string> elements;
    elements.reserve(value.size());
    for (const auto& element : value)
        elements.push_back(FormatValue(element, singlePrecision));
    return std::format("({})", Join(elements, ", "));
}

void DescribeObject(std::string& result, const Json& before, const Json& after, const std::string& path)
{
    if (before == after)
        return;
    if (!before.is_object() || !after.is_object())
    {
        AppendChange(result, path, FormatValue(before, true), FormatValue(after, true), true);
        return;
    }
    for (const auto& [key, value] : before.items())
    {
        const std::string label = path.empty() ? IdentifierToWords(key) : std::format("{}.{}", path, IdentifierToWords(key));
        if (after.contains(key))
            DescribeObject(result, value, after[key], label);
        else
        {
            if (value.is_object())
                DescribeObject(result, value, Json::object(), label);
            else
                AppendChange(result, label, FormatValue(value, true), "(absent)", true);
        }
    }
    for (const auto& [key, value] : after.items())
    {
        if (before.contains(key))
            continue;
        const std::string label = path.empty() ? IdentifierToWords(key) : std::format("{}.{}", path, IdentifierToWords(key));
        if (value.is_object())
            DescribeObject(result, Json::object(), value, label);
        else
            AppendChange(result, label, "(absent)", FormatValue(value, true), true);
    }
}

std::string FormatVector(const Mathematics::Vector3& value, int significantDigits = 6)
{
    return std::format("({}, {}, {})", FormatFloat(value.x, significantDigits), FormatFloat(value.y, significantDigits),
        FormatFloat(value.z, significantDigits));
}

Mathematics::Vector3 RotationDegrees(const Components::Transform& transform)
{
    Mathematics::Vector3 degrees;
    Components::EulerXYZDegreesFromQuaternion(transform.GetRotation(), degrees.x, degrees.y, degrees.z);
    return degrees;
}

Json ComponentValues(std::span<const ECS::FieldInfo> fields, std::span<const std::uint8_t> bytes)
{
    if (bytes.empty())
        return Json::object();
    std::string json;
    ECS::AppendComponentJson(json, fields, reinterpret_cast<const std::byte*>(bytes.data()));
    return Json::parse(json, nullptr, false);
}

std::string FormatName(std::span<const std::uint8_t> bytes)
{
    if (bytes.empty())
        return "(unnamed)";
    if (bytes.size() != sizeof(Components::Name))
        return "(unavailable)";
    Components::Name value{};
    std::memcpy(&value, bytes.data(), sizeof(value));
    return Json(std::string(value.View())).dump(-1, ' ', false, Json::error_handler_t::replace);
}

bool FieldFits(const ECS::FieldInfo& field, std::span<const std::uint8_t> bytes)
{
    return field.Offset <= bytes.size() && field.Size <= bytes.size() - field.Offset;
}

std::string FieldLabel(const ECS::FieldInfo& field)
{
    if (!field.Label.empty())
        return std::string(field.Label);
    std::string_view name = field.Name;
    if (field.Type == ECS::FieldTypeId::AssetGuid)
    {
        constexpr std::string_view kAssetSuffix = "AssetGuid";
        constexpr std::string_view kGuidSuffix = "Guid";
        if (name.ends_with(kAssetSuffix))
            name.remove_suffix(kAssetSuffix.size());
        else if (name.ends_with(kGuidSuffix))
            name.remove_suffix(kGuidSuffix.size());
    }
    return IdentifierToWords(name);
}

std::string FormatFieldValue(const Json& value, const ECS::FieldInfo& field)
{
    if (field.Type == ECS::FieldTypeId::AssetGuid && value.is_string() && value.get_ref<const std::string&>().empty())
        return "(none)";
    return FormatValue(value);
}
} // namespace

std::string HistoryValueFormatter::DescribeComponent(ECS::ComponentTypeId typeId,
    std::span<const std::uint8_t> before, std::span<const std::uint8_t> after)
{
    std::string result;
    if (typeId == ECS::GetComponentTypeId<Components::Name>())
    {
        AppendChange(result, "Name", FormatName(before), FormatName(after));
        return result;
    }
    if (before.empty() != after.empty())
    {
        const std::string label = IdentifierToWords(UnqualifiedName(ECS::ComponentFieldRegistry::GetCanonicalName(typeId)));
        AppendChange(result, label.empty() ? "Component" : label,
            before.empty() ? "absent" : "present", after.empty() ? "absent" : "present");
        return result;
    }
    if (typeId == ECS::GetComponentTypeId<Components::Transform>() &&
        before.size() == sizeof(Components::Transform) && after.size() == sizeof(Components::Transform))
    {
        Components::Transform oldTransform;
        Components::Transform newTransform;
        std::memcpy(&oldTransform, before.data(), sizeof(oldTransform));
        std::memcpy(&newTransform, after.data(), sizeof(newTransform));
        AppendChange(result, "Position", FormatVector(oldTransform.GetPosition(), 0), FormatVector(newTransform.GetPosition(), 0));
        AppendChange(result, "Rotation (degrees)", FormatVector(RotationDegrees(oldTransform)), FormatVector(RotationDegrees(newTransform)));
        AppendChange(result, "Scale", FormatVector(oldTransform.GetScale()), FormatVector(newTransform.GetScale()));
        if (!result.empty())
            return result;
    }
    std::vector<ECS::FieldInfo> fields;
    for (const auto& field : ECS::ComponentFieldRegistry::Get(typeId))
    {
        if (ECS::HasAnyFlag(field.Flags, ECS::FieldFlags::Hidden | ECS::FieldFlags::Transient))
            continue;
        if (FieldFits(field, before) && FieldFits(field, after))
            fields.push_back(field);
    }
    const Json oldValues = ComponentValues(fields, before);
    const Json newValues = ComponentValues(fields, after);
    if (oldValues.is_discarded() || newValues.is_discarded())
        return {};
    for (const auto& field : fields)
    {
        const std::string key(field.Name);
        if (oldValues.contains(key) && newValues.contains(key) && oldValues[key] == newValues[key])
            continue;
        AppendChange(result, FieldLabel(field), oldValues.contains(key) ? FormatFieldValue(oldValues[key], field) : "(absent)",
            newValues.contains(key) ? FormatFieldValue(newValues[key], field) : "(absent)", true);
    }
    return result;
}

std::string HistoryValueFormatter::DescribeMaterial(std::span<const std::uint8_t> before,
    std::span<const std::uint8_t> after, const std::filesystem::path& path)
{
    MaterialAsset oldMaterial(GUID::Null(), path);
    MaterialAsset newMaterial(GUID::Null(), path);
    if (!oldMaterial.LoadFromData(std::vector<std::uint8_t>(before.begin(), before.end())) ||
        !newMaterial.LoadFromData(std::vector<std::uint8_t>(after.begin(), after.end())))
        return {};
    const Json oldValues = SerializeMaterialDocument(oldMaterial.GetDocument());
    const Json newValues = SerializeMaterialDocument(newMaterial.GetDocument());
    std::string result;
    DescribeObject(result, oldValues, newValues, "Value");
    return result;
}
} // namespace GameEngine::Editor
