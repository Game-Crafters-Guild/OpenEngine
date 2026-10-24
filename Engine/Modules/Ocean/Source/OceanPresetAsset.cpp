#include "Ocean/OceanPresetAsset.h"

#include "Components/Rendering/Ocean.h"
#include "AssetCore/SharedFileRead.h"
#include "AssetCore/GUID.h"
#include "Components/RenderLayerMaskReflectionTraits.h"
#include "ECS/ECSTemplates.h"
#include "ECS/FieldAccess.h"
#include <cctype>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <span>
#include <type_traits>
#include <nlohmann/json.hpp>

namespace GameEngine::Ocean
{
namespace
{
void SetOceanPresetError(std::string* error, const char* text)
{
    if (error)
        *error = text;
}

// The identities of the fields a binding keeps, without its unused (zero) entries,
// so each field compares against the short list.
std::span<const StringId> CollectOverriddenFields(
    std::span<const Components::OceanPresetOverride> overrides,
    std::span<StringId, Components::kOceanPresetOverrideCapacity> storage)
{
    size_t count = 0u;
    for (const Components::OceanPresetOverride& entry :
         overrides.first(std::min(overrides.size(), storage.size())))
    {
        if (entry.FieldIdentifier != 0u)
            storage[count++] = entry.FieldIdentifier;
    }
    return storage.first(count);
}

// A preset number as the element type of its field kind: the render layer word takes the
// number as its bits; every other kind is the number itself.
template <typename TElement, typename TNumber>
TElement ToElement(TNumber number)
{
    if constexpr (std::is_same_v<TElement, Rendering::RenderLayerMask>)
        return Rendering::RenderLayerMask{static_cast<uint32>(number)};
    else
        return static_cast<TElement>(number);
}

template <typename TElement>
bool ReadScalar(const OceanPresetValue& value, TElement& out)
{
    return std::visit([&](const auto& stored) -> bool {
        using U = std::decay_t<decltype(stored)>;
        if constexpr (std::is_same_v<U, bool> || std::is_integral_v<U> ||
                      std::is_floating_point_v<U>)
        {
            out = ToElement<TElement>(stored);
            return true;
        }
        else
        {
            return false;
        }
    }, value);
}

// One element: an integer at the width of the field's own kind, every other kind as TElement.
template <typename TElement>
bool WriteElement(const ECS::FieldInfo& field, std::span<std::byte> component, uint32 element,
                  const TElement& number)
{
    if constexpr (std::is_same_v<TElement, int64>)
        return ECS::WriteFieldInteger(field, component, element, number);
    else
        return ECS::WriteFieldElement(field, component, element, number);
}

// Every element of the field from a number array of at least its count, or its one element from
// a scalar.
template <typename TElement>
bool WriteNumbers(const ECS::FieldInfo& field, const OceanPresetValue& value, std::span<std::byte> component)
{
    const uint32 count = field.Size / ECS::FieldElementSize(field.Type);
    if (const auto* array = std::get_if<OceanPresetNumberArray>(&value))
    {
        if (array->size() < count)
            return false;
        for (uint32 i = 0u; i < count; ++i)
        {
            if (!WriteElement(field, component, i, ToElement<TElement>((*array)[i])))
                return false;
        }
        return true;
    }
    if (count != 1u)
        return false;
    TElement scalar{};
    if (!ReadScalar(value, scalar))
        return false;
    return WriteElement(field, component, 0u, scalar);
}

// Every element of a Color field from a number array of four channels (r, g, b, a) per element.
bool WriteColors(const ECS::FieldInfo& field, const OceanPresetValue& value, std::span<std::byte> component)
{
    constexpr size_t kChannels = 4u;
    const auto* array = std::get_if<OceanPresetNumberArray>(&value);
    const uint32 count = field.Size / ECS::FieldElementSize(field.Type);
    if (!array || array->size() < count * kChannels)
        return false;
    for (uint32 element = 0u; element < count; ++element)
    {
        const double* channels = array->data() + element * kChannels;
        const ColorLinear color(static_cast<float32>(channels[0]), static_cast<float32>(channels[1]),
                                static_cast<float32>(channels[2]), static_cast<float32>(channels[3]));
        if (!ECS::WriteFieldElement(field, component, element, color))
            return false;
    }
    return true;
}

bool WriteFieldValue(const ECS::FieldInfo& field, const OceanPresetValue& value,
                     std::byte* component, size_t componentBytes)
{
    if (static_cast<size_t>(field.Offset) + field.Size > componentBytes)
        return false;
    const std::span<std::byte> bytes(component, componentBytes);
    std::byte* destination = component + field.Offset;
    using ECS::FieldTypeId;
    switch (field.Type)
    {
    case FieldTypeId::Bool: return WriteNumbers<bool>(field, value, bytes);
    case FieldTypeId::Int8:
    case FieldTypeId::Int16:
    case FieldTypeId::Int32:
    case FieldTypeId::Int64:
    case FieldTypeId::UInt8:
    case FieldTypeId::UInt16:
    case FieldTypeId::UInt32:
    case FieldTypeId::UInt64: return WriteNumbers<int64>(field, value, bytes);
    case FieldTypeId::RenderLayerMask: return WriteNumbers<Rendering::RenderLayerMask>(field, value, bytes);
    case FieldTypeId::Float: return WriteNumbers<float32>(field, value, bytes);
    case FieldTypeId::Double: return WriteNumbers<double>(field, value, bytes);
    case FieldTypeId::Color: return WriteColors(field, value, bytes);
    case FieldTypeId::AssetGuid:
        if (const auto* text = std::get_if<std::string>(&value); text && field.Size == sizeof(GUID))
        {
            constexpr size_t kGuidHexDigits = GUID::kSize * 2u;
            std::string digits = *text;
            digits.erase(std::remove(digits.begin(), digits.end(), '-'), digits.end());
            if (!digits.empty() && (digits.size() != kGuidHexDigits || !std::all_of(digits.begin(), digits.end(),
                [](unsigned char character) { return std::isxdigit(character) != 0; }))) return false;
            const GUID guid = digits.empty() ? GUID::Null() : GUID(digits);
            return ECS::WriteFieldElement(field, bytes, 0u, guid);
        }
        return false;
    case FieldTypeId::String:
        if (const auto* stringValue = std::get_if<std::string>(&value))
        {
            const size_t copy = std::min(stringValue->size(), static_cast<size_t>(field.Size - 1u));
            std::memset(destination, 0, field.Size);
            std::memcpy(destination, stringValue->data(), copy);
            return true;
        }
        return false;
    default: return false;
    }
}

nlohmann::json ToJson(const OceanPresetValue& value)
{
    return std::visit([](const auto& stored) -> nlohmann::json { return stored; }, value);
}

OceanPresetValue FromJson(const nlohmann::json& value)
{
    if (value.is_boolean()) return value.get<bool>();
    if (value.is_number_unsigned()) return value.get<uint64>();
    if (value.is_number_integer()) return value.get<int64>();
    if (value.is_number_float()) return value.get<double>();
    if (value.is_string()) return value.get<std::string>();
    if (value.is_array()) return value.get<OceanPresetNumberArray>();
    return std::string{};
}
} // namespace

void OceanPresetAsset::Set(std::string fieldPath, OceanPresetValue value)
{
    const StringId fieldIdentifier = HashStringId(fieldPath);
    m_Values[std::move(fieldPath)] = Entry{std::move(value), fieldIdentifier};
    ++Revision;
}

bool OceanPresetAsset::Remove(std::string_view fieldPath)
{
    const size_t removed = m_Values.erase(std::string(fieldPath));
    if (removed)
        ++Revision;
    return removed != 0u;
}

const OceanPresetValue* OceanPresetAsset::Find(std::string_view fieldPath) const
{
    const auto found = m_Values.find(std::string(fieldPath));
    return found == m_Values.end() ? nullptr : &found->second.Value;
}

bool OceanPresetAsset::SaveJson(const std::filesystem::path& path, std::string* error) const
{
    nlohmann::json values = nlohmann::json::object();
    for (const auto& [key, entry] : m_Values)
        values[key] = ToJson(entry.Value);
    nlohmann::json document{{"FormatVersion", kFormatVersion},
                            {"Revision", Revision}, {"Values", std::move(values)}};
    std::ofstream stream(path, std::ios::trunc);
    if (!stream)
    {
        SetOceanPresetError(error, "failed to open ocean preset for writing");
        return false;
    }
    stream << document.dump(2) << '\n';
    return static_cast<bool>(stream);
}

bool OceanPresetAsset::LoadJson(const std::filesystem::path& path, std::string* error)
{
    String text;
    if (!ReadFileTextShared(path, text))
    {
        SetOceanPresetError(error, "failed to open ocean preset");
        return false;
    }
    nlohmann::json document = nlohmann::json::parse(text, nullptr, false);
    if (document.is_discarded())
    {
        SetOceanPresetError(error, "invalid ocean preset JSON");
        return false;
    }
    if (document.value("FormatVersion", 0u) != kFormatVersion ||
        !document.contains("Values") || !document["Values"].is_object())
    {
        SetOceanPresetError(error, "unsupported or malformed ocean preset");
        return false;
    }
    std::unordered_map<std::string, Entry> loaded;
    for (auto it = document["Values"].begin(); it != document["Values"].end(); ++it)
    {
        if (it.value().is_object() || it.value().is_null())
            continue;
        loaded[it.key()] = Entry{FromJson(it.value()), HashStringId(it.key())};
    }
    m_Values = std::move(loaded);
    Revision = document.value("Revision", Revision + 1u);
    return true;
}

uint32 OceanPresetAsset::Apply(std::string_view prefix, ECS::ComponentTypeId typeId,
                               void* component, size_t componentBytes,
                               std::span<const Components::OceanPresetOverride> overrides) const
{
    if (!component)
        return 0u;
    std::array<StringId, Components::kOceanPresetOverrideCapacity> overriddenStorage;
    const std::span<const StringId> overridden =
        CollectOverriddenFields(overrides, overriddenStorage);
    const std::span<const ECS::FieldInfo> fields = ECS::ComponentFieldRegistry::Get(typeId);
    uint32 applied = 0u;
    // One path buffer for every field: it stops allocating once it holds the longest.
    std::string fieldPath(prefix);
    fieldPath += '.';
    const size_t rootLength = fieldPath.size();
    for (const ECS::FieldInfo& field : fields)
    {
        fieldPath.resize(rootLength);
        fieldPath += field.Name;
        const auto found = m_Values.find(fieldPath);
        if (found == m_Values.end() ||
            std::ranges::find(overridden, found->second.FieldIdentifier) != overridden.end())
        {
            continue;
        }
        if (WriteFieldValue(field, found->second.Value,
                            static_cast<std::byte*>(component), componentBytes))
        {
            ++applied;
        }
    }
    return applied;
}

std::span<const Components::OceanPresetOverride> PresetOverridesOf(const ECS::World& world,
                                                                   ECS::EntityHandle entity)
{
    const auto* binding = world.GetComponent<Components::OceanPresetBinding>(entity);
    if (!binding)
        return {};
    return binding->Overrides;
}

void OceanPresetInstance::SetOverride(std::string fieldPath, OceanPresetValue value)
{
    m_Overrides[std::move(fieldPath)] = std::move(value);
}

bool OceanPresetInstance::ResetField(std::string_view fieldPath)
{
    return m_Overrides.erase(std::string(fieldPath)) != 0u;
}

void OceanPresetInstance::ResetAll()
{
    m_Overrides.clear();
}

bool OceanPresetInstance::IsOverridden(std::string_view fieldPath) const
{
    return m_Overrides.find(std::string(fieldPath)) != m_Overrides.end();
}

const OceanPresetValue* OceanPresetInstance::Resolve(std::string_view fieldPath) const
{
    const auto overrideValue = m_Overrides.find(std::string(fieldPath));
    if (overrideValue != m_Overrides.end())
        return &overrideValue->second;
    return m_Base ? m_Base->Find(fieldPath) : nullptr;
}

} // namespace GameEngine::Ocean
