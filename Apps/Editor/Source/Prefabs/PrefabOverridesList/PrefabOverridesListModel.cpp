#include "Prefabs/PrefabOverridesList/PrefabOverridesListModel.h"

#include "Editor/Entities/EntityDisplayName.h"
#include "Inspectors/DefaultComponentInspector.h"
#include "Prefabs/PrefabHierarchyRows.h"
#include "Prefabs/PrefabInstanceDescriptions.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Components/Name.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/Reflection.h"
#include "ECS/World.h"
#include "Mathematics/Vector2.h"
#include "Mathematics/Vector4.h"
#include "Scene/SceneIO.h"
#include "Scene/SceneSchemaRegistry.h"
#include "Types/FormatNumber.h"
#include "Types/StringUtils.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <span>
#include <string_view>
#include <vector>

namespace GameEngine::Editor
{
namespace
{

constexpr std::string_view kTransformToken = "Transform";
constexpr std::string_view kNameToken = "Name";

// Reads a value of type T at `offset` of a component's bytes; false when the bytes are too short.
template <typename T>
bool ReadAt(const std::vector<std::uint8_t>& bytes, std::size_t offset, T& out)
{
    if (offset + sizeof(T) > bytes.size())
        return false;
    std::memcpy(&out, bytes.data() + offset, sizeof(T));
    return true;
}

// Reads an integer of type T at `offset` and writes it as text.
template <typename T>
std::string IntegerAt(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    T value{};
    return ReadAt(bytes, offset, value) ? std::to_string(value) : std::string();
}

// Axes as the Inspector's vector fields show them: "(1, 0.5, 0)".
std::string FormatAxes(std::span<const float> axes)
{
    std::vector<std::string> parts;
    for (const float axis : axes)
        parts.push_back(FormatFloat(axis));
    return "(" + Join(parts, ", ") + ")";
}


// The `count` floats at `offset`: one number, or the axes of a float array such as a color. Empty when the
// bytes are too short.
std::string FloatsAt(const std::vector<std::uint8_t>& bytes, std::size_t offset, std::size_t count)
{
    std::vector<float> axes(count);
    for (std::size_t index = 0; index < count; ++index)
        if (!ReadAt(bytes, offset + index * sizeof(float), axes[index]))
            return {};
    if (count == 1)
        return FormatFloat(axes.front());
    return FormatAxes(axes);
}

// A rotation as the Inspector shows it: Euler angles in degrees, XYZ order.
std::string FormatRotation(const Quaternion& rotation)
{
    float32 x = 0.0f;
    float32 y = 0.0f;
    float32 z = 0.0f;
    Components::EulerXYZDegreesFromQuaternion(rotation, x, y, z);
    return FormatAxes(std::array{x, y, z});
}

std::string AssetNameOf(const GUID& guid)
{
    if (guid.IsNull())
        return "None";
    AssetMetadata metadata;
    if (!EngineCore::GetInstance().GetAssetManager().GetRegistry().TryGetAssetMetadata(guid, metadata))
        return "Missing asset";
    return std::filesystem::path(metadata.Path).stem().string();
}

// The row label of a Transform field for its scene key: "Transform: " and the Inspector's label of the field.
std::string TransformLabelOf(std::string_view key)
{
    if (EqualsIgnoreCase(key, "position"))
        return "Transform: Local Position";
    if (EqualsIgnoreCase(key, "rotation"))
        return "Transform: Local Rotation";
    if (EqualsIgnoreCase(key, "scale"))
        return "Transform: Local Scale";
    return "Transform: " + IdentifierToWords(key);
}

void DescribeTransformValue(const ECS::World& prefabWorld, ECS::EntityHandle prefabEntity, std::string_view key,
                            PrefabOverrideRow& row)
{
    const auto* transform = prefabWorld.GetComponent<Components::Transform>(prefabEntity);
    if (!transform)
        return;
    if (EqualsIgnoreCase(key, "position"))
    {
        const Vector3 position = transform->GetPosition() - Mathematics::WorldPosition{};
        row.PrefabValue = FormatAxes(std::array{position.x, position.y, position.z});
    }
    else if (EqualsIgnoreCase(key, "rotation"))
    {
        row.PrefabValue = FormatRotation(transform->GetRotation());
    }
    else if (EqualsIgnoreCase(key, "scale"))
    {
        const Vector3 scale = transform->GetScale();
        row.PrefabValue = FormatAxes(std::array{scale.x, scale.y, scale.z});
    }
}

// The value of one reflected field of a component's bytes as the Inspector shows it. A type that the Inspector shows
// with a control of its own (a matrix, raw bytes, a string, a layer mask) gives no value.
void DescribeReflectedValue(const ECS::World& prefabWorld, const ECS::FieldInfo& field,
                            const std::vector<std::uint8_t>& bytes, PrefabOverrideRow& row)
{
    const std::size_t offset = field.Offset;
    switch (field.Type)
    {
        case ECS::FieldTypeId::Bool:
            if (bool value = false; ReadAt(bytes, offset, value))
                row.PrefabValue = value ? "On" : "Off";
            break;
        case ECS::FieldTypeId::Int8: row.PrefabValue = IntegerAt<std::int8_t>(bytes, offset); break;
        case ECS::FieldTypeId::Int16: row.PrefabValue = IntegerAt<std::int16_t>(bytes, offset); break;
        case ECS::FieldTypeId::Int32: row.PrefabValue = IntegerAt<std::int32_t>(bytes, offset); break;
        case ECS::FieldTypeId::Int64: row.PrefabValue = IntegerAt<std::int64_t>(bytes, offset); break;
        case ECS::FieldTypeId::UInt8: row.PrefabValue = IntegerAt<std::uint8_t>(bytes, offset); break;
        case ECS::FieldTypeId::UInt16: row.PrefabValue = IntegerAt<std::uint16_t>(bytes, offset); break;
        case ECS::FieldTypeId::UInt32: row.PrefabValue = IntegerAt<std::uint32_t>(bytes, offset); break;
        case ECS::FieldTypeId::UInt64: row.PrefabValue = IntegerAt<std::uint64_t>(bytes, offset); break;
        case ECS::FieldTypeId::Float:
            row.PrefabValue = FloatsAt(bytes, offset, field.Size / sizeof(float));
            break;
        case ECS::FieldTypeId::Double:
            if (double value = 0.0; ReadAt(bytes, offset, value))
                row.PrefabValue = FormatFloat(static_cast<float>(value));
            break;
        case ECS::FieldTypeId::Vec2:
            if (Mathematics::Vector2 value; ReadAt(bytes, offset, value))
                row.PrefabValue = FormatAxes(std::array{value.x, value.y});
            break;
        case ECS::FieldTypeId::Vec3:
            if (Vector3 value; ReadAt(bytes, offset, value))
                row.PrefabValue = FormatAxes(std::array{value.x, value.y, value.z});
            break;
        case ECS::FieldTypeId::Vec4:
            if (Mathematics::Vector4 value; ReadAt(bytes, offset, value))
                row.PrefabValue = FormatAxes(std::array{value.x, value.y, value.z, value.w});
            break;
        case ECS::FieldTypeId::Quat:
            if (Quaternion value; ReadAt(bytes, offset, value))
                row.PrefabValue = FormatRotation(value);
            break;
        case ECS::FieldTypeId::Color:
            if (ColorLinear value; ReadAt(bytes, offset, value))
                row.PrefabColor = value;
            break;
        case ECS::FieldTypeId::AssetGuid:
            if (GUID::Data data{}; ReadAt(bytes, offset, data))
                row.PrefabValue = AssetNameOf(GUID(data));
            break;
        case ECS::FieldTypeId::EntityHandle:
            if (std::uint32_t id = 0; ReadAt(bytes, offset, id))
                row.PrefabValue = id == 0 ? std::string("None") : EntityDisplayName(prefabWorld, ECS::EntityHandle(id));
            break;
        default:
            break;
    }
}

// The value of the changed field `entry` of `entity` as the Inspector shows it, in row.PrefabValue and
// row.PrefabColor.
void DescribeFieldValue(const ECS::World& world, ECS::EntityHandle entity, const Scene::PrefabOverride& entry,
                        PrefabOverrideRow& row)
{
    if (EqualsIgnoreCase(entry.Component, kTransformToken))
    {
        DescribeTransformValue(world, entity, entry.Field, row);
        return;
    }
    if (EqualsIgnoreCase(entry.Component, kNameToken))
    {
        if (const auto* name = world.GetComponent<Components::Name>(entity))
            row.PrefabValue = std::format("'{}'", name->View());
        return;
    }
    const Scene::ISceneComponentSchema* schema = Scene::SceneSchemaRegistry::Find(entry.Component);
    if (!schema)
        return;
    const ECS::ComponentTypeId type = Scene::SceneSchemaRegistry::ComponentTypeForSchema(*schema);
    const std::string_view fieldName = InspectorFieldForSceneKey(type, entry.Field);
    // A key with an index names an array element, which has no value of its own here.
    if (fieldName.empty() || !EqualsIgnoreCase(entry.Field, fieldName))
        return;
    for (const ECS::FieldInfo& field : ECS::ComponentFieldRegistry::Get(type))
    {
        std::vector<std::uint8_t> bytes;
        if (field.Name == fieldName && world.CaptureComponentBytes(entity, type, bytes))
            DescribeReflectedValue(world, field, bytes, row);
    }
}

// The row label of the changed field `entry`.
std::string FieldLabelOf(const Scene::PrefabOverride& entry)
{
    if (EqualsIgnoreCase(entry.Component, kTransformToken))
        return TransformLabelOf(entry.Field);
    if (EqualsIgnoreCase(entry.Component, kNameToken))
        return "Name";
    if (const Scene::ISceneComponentSchema* schema = Scene::SceneSchemaRegistry::Find(entry.Component))
    {
        const ECS::ComponentTypeId type = Scene::SceneSchemaRegistry::ComponentTypeForSchema(*schema);
        const std::string_view fieldName = InspectorFieldForSceneKey(type, entry.Field);
        for (const ECS::FieldInfo& field : ECS::ComponentFieldRegistry::Get(type))
            if (!fieldName.empty() && field.Name == fieldName)
                return IdentifierToWords(entry.Component) + ": " + FieldDisplayName(field);
    }
    return IdentifierToWords(entry.Component) + ": " + IdentifierToWords(entry.Field);
}

// The label and the prefab value of a changed field. The template entity holds the prefab's values.
void DescribeField(ECS::World& world, ECS::EntityHandle entity, const Scene::PrefabOverride& entry,
                   PrefabOverrideRow& row)
{
    row.Label = FieldLabelOf(entry);
    const Scene::PrefabTemplateEntity prefab = Scene::FindPrefabTemplateEntity(world, entity);
    if (!prefab.World)
        return;
    DescribeFieldValue(*prefab.World, prefab.Entity, entry, row);
    PrefabOverrideRow instance;
    DescribeFieldValue(world, entity, entry, instance);
    // A field that keeps a record can hold the prefab's value.
    row.SameAsPrefab = !row.PrefabValue.empty() && instance.PrefabValue == row.PrefabValue;
}

// The name of the prefab's entity at `address` in the prefab of the instance that holds `entity`; the address when
// the prefab has no entity there or the entity has no name.
std::string PrefabEntityNameAt(ECS::World& world, ECS::EntityHandle entity, const std::string& address)
{
    const Scene::PrefabTemplateEntity prefab = Scene::FindPrefabTemplateEntityAt(world, entity, address);
    const auto* name = prefab.World ? prefab.World->GetComponent<Components::Name>(prefab.Entity) : nullptr;
    return name && !name->View().empty() ? std::string(name->View()) : address;
}

PrefabOverrideRow DescribeRow(ECS::World& world, ECS::EntityHandle entity, const Scene::PrefabOverride& entry)
{
    PrefabOverrideRow row;
    row.Override = entry;
    switch (entry.Kind)
    {
        case Scene::PrefabOverrideKind::Field:
            row.Kind = PrefabOverrideRowKind::ChangedField;
            DescribeField(world, entity, entry, row);
            break;
        case Scene::PrefabOverrideKind::AddedComponent:
            row.Kind = PrefabOverrideRowKind::AddedComponent;
            row.Label = IdentifierToWords(entry.Component);
            break;
        case Scene::PrefabOverrideKind::RemovedComponent:
            row.Kind = PrefabOverrideRowKind::RemovedComponent;
            row.Label = IdentifierToWords(entry.Component);
            break;
        case Scene::PrefabOverrideKind::RemovedChild:
            row.Kind = PrefabOverrideRowKind::RemovedEntity;
            row.Label = PrefabEntityNameAt(world, entity, entry.Field);
            break;
    }
    return row;
}

// The row of an orphaned override: the component and the field that the line names, labeled as a changed field is.
std::string DescribeStaleLine(const Scene::SceneDegradation::Skip& skip)
{
    if (skip.component.empty())
        return "Removed entity";
    if (skip.field.empty())
        return IdentifierToWords(skip.component);
    if (EqualsIgnoreCase(skip.component, kTransformToken))
        return TransformLabelOf(skip.field);
    if (EqualsIgnoreCase(skip.component, kNameToken))
        return "Name";
    return IdentifierToWords(skip.component) + ": " + IdentifierToWords(skip.field);
}

// True for an orphaned override that the load recorded on the instance's root. A line under a child that the
// instance removes is not one: the child's Removed row stands for it.
bool IsStaleRow(const Scene::SceneDegradation::Skip& skip, ECS::EntityHandle root)
{
    return skip.kind == Scene::SceneDegradation::SkipKind::OrphanedOverride && skip.entityHandle == root;
}

// One amber group for each address that the instance's orphaned overrides name, in the order of the load report.
void AppendStaleGroups(ECS::World& world, ECS::EntityHandle root, std::span<const Scene::SceneDegradation::Skip> loadSkips,
                       std::vector<PrefabOverrideGroup>& groups)
{
    const std::size_t firstStale = groups.size();
    // The address of each stale group, from groups[firstStale] on.
    std::vector<std::string_view> addresses;
    for (std::size_t index = 0; index < loadSkips.size(); ++index)
    {
        const Scene::SceneDegradation::Skip& skip = loadSkips[index];
        if (!IsStaleRow(skip, root))
            continue;
        const std::size_t at = static_cast<std::size_t>(std::ranges::find(addresses, skip.address) - addresses.begin());
        if (at == addresses.size())
        {
            addresses.push_back(skip.address);
            groups.push_back({{}, PrefabEntityNameAt(world, root, skip.address), true, {}});
        }
        groups[firstStale + at].Rows.push_back(
            {PrefabOverrideRowKind::Stale, DescribeStaleLine(skip), {}, {}, {}, index});
    }
}

} // namespace

PrefabOverrideCounts PrefabOverridesListModel::Count(ECS::World& world, ECS::EntityHandle entity,
                                                     std::span<const Scene::SceneDegradation::Skip> loadSkips)
{
    PrefabOverrideCounts counts;
    const DescribedPrefabInstance* instance = PrefabInstanceDescriptions::Get().FindInstance(world, entity);
    if (!instance)
        return counts;
    for (const Scene::PrefabInstanceEntity& described : instance->Entities)
        counts.Overrides += (described.AddedOnInstance ? 1 : 0) + described.Overrides.size();
    for (const Scene::SceneDegradation::Skip& skip : loadSkips)
        counts.Stale += IsStaleRow(skip, instance->Root) ? 1 : 0;
    return counts;
}

PrefabOverridesListModel PrefabOverridesListModel::Describe(ECS::World& world, ECS::EntityHandle entity,
                                                            std::span<const Scene::SceneDegradation::Skip> loadSkips)
{
    PrefabOverridesListModel model;
    const DescribedPrefabInstance* instance = PrefabInstanceDescriptions::Get().FindInstance(world, entity);
    if (!instance)
        return model;
    model.Root = instance->Root;
    model.InstanceName = EntityDisplayName(world, instance->Root);
    model.PrefabName = PrefabNameOf(world, instance->Root);
    // The descriptions can change while the rows read the prefab values, so the entities are copied first.
    const std::vector<Scene::PrefabInstanceEntity> entities = instance->Entities;
    for (const Scene::PrefabInstanceEntity& described : entities)
    {
        PrefabOverrideGroup group{described.Entity, EntityDisplayName(world, described.Entity), false, {}};
        if (described.AddedOnInstance)
            group.Rows.push_back({PrefabOverrideRowKind::AddedEntity, "Entity", {}, {}, {}, 0});
        for (const Scene::PrefabOverride& entry : described.Overrides)
            group.Rows.push_back(DescribeRow(world, described.Entity, entry));
        if (!group.Rows.empty())
            model.Groups.push_back(std::move(group));
    }
    AppendStaleGroups(world, model.Root, loadSkips, model.Groups);
    return model;
}

} // namespace GameEngine::Editor
