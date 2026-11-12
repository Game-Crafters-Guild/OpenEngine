#include "AI/RecordStore.h"
#include "AIECS/Components/AiGroup.h"
#include "AiGroup/AiGroupStore.h"
#include "ECS/ECSTemplates.h"
#include "Logger/Logger.h"
#include "Scene/AiSceneText.h"
#include "Scene/SceneSchemaRegistry.h"
#include "Scene/SceneValue.h"

#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace GameEngine::AIECS
{
namespace
{

// A record is (subject, owner, kind, x, y, z, strength, value0, value1, firstSeen, lastSeen).
constexpr size_t kRecordFields = 11;

const AiGroupStore::Group* FindKnowledge(const ECS::World& world, ECS::EntityHandle entity, const Components::AiGroup& group)
{
    const AiGroupStore* store = AiGroupStore::FindForWorld(world.GetWorldId());
    return store != nullptr ? store->Find(entity, group.Knowledge) : nullptr;
}

// The group's knowledge entry, made when the group has none yet.
AiGroupStore::Group& FindOrMakeKnowledge(ECS::World& world, ECS::EntityHandle entity)
{
    if (world.GetComponent<Components::AiGroup>(entity) == nullptr)
        world.AddComponentImmediate(entity, Components::AiGroup{});
    Components::AiGroup& group = *world.GetComponentForWrite<Components::AiGroup>(entity);
    if (AiGroupStore* store = AiGroupStore::FindForWorld(world.GetWorldId()))
    {
        if (AiGroupStore::Group* found = store->Find(entity, group.Knowledge))
            return *found;
    }
    group.Knowledge = AiGroupStore::CreateInWorld(world.GetWorldId(), entity);
    return *AiGroupStore::FindForWorld(world.GetWorldId())->Find(entity, group.Knowledge);
}

std::string FormatRecord(const Scene::SceneWriteContext& ctx, const AI::RecordStore& records, uint32 index,
                         const std::string& subject, bool idsAreEntities)
{
    const Mathematics::Vector3& position = records.GetPositions()[index];
    return "(" + subject + ", " + FormatAiValue(ctx, records.GetOwners()[index], idsAreEntities) + ", " +
           std::to_string(records.GetKinds()[index]) + ", " + Scene::FormatFloat(position.x) + ", " +
           Scene::FormatFloat(position.y) + ", " + Scene::FormatFloat(position.z) + ", " +
           Scene::FormatFloat(records.GetStrengths()[index]) + ", " + Scene::FormatFloat(records.GetValues0()[index]) +
           ", " + Scene::FormatFloat(records.GetValues1()[index]) + ", " +
           std::to_string(records.GetFirstSeen()[index]) + ", " + std::to_string(records.GetLastSeen()[index]) + ")";
}

bool ReadNumber(const Scene::SceneValue& item, float32& out)
{
    if (item.Kind == Scene::SceneValueKind::Int)
        out = static_cast<float32>(item.IntValue);
    else if (item.Kind == Scene::SceneValueKind::Float)
        out = static_cast<float32>(item.FloatValue);
    else
        return false;
    return true;
}

// Reads one record. `outKeep` is false for a record whose subject entity is not in the load.
bool ReadRecord(const Scene::SceneReadContext& ctx, const Scene::SceneValue& tuple, AI::RecordStore::Record& out,
                bool& outKeep)
{
    if (tuple.Kind != Scene::SceneValueKind::Tuple || tuple.Items.size() != kRecordFields)
        return false;
    const std::vector<Scene::SceneValue>& items = tuple.Items;
    uint32 kind = 0;
    const bool read = ReadAiValue(ctx, items[0], out.Last.Subject) && ReadAiValue(ctx, items[1], out.Last.Owner) &&
                      ReadAiValue(ctx, items[2], kind) && kind <= std::numeric_limits<uint16>::max() &&
                      ReadNumber(items[3], out.Last.Position.x) &&
                      ReadNumber(items[4], out.Last.Position.y) && ReadNumber(items[5], out.Last.Position.z) &&
                      ReadNumber(items[6], out.Last.Strength) && ReadNumber(items[7], out.Last.Value0) &&
                      ReadNumber(items[8], out.Last.Value1) && ReadAiValue(ctx, items[9], out.FirstSeen) &&
                      ReadAiValue(ctx, items[10], out.LastSeen);
    out.Last.Kind = static_cast<uint16>(kind);
    outKeep = items[0].Kind != Scene::SceneValueKind::String || ECS::EntityHandle(out.Last.Subject).IsValid();
    return read;
}

bool ReadRecords(const Scene::SceneReadContext& ctx, std::string_view value, std::vector<AI::RecordStore::Record>& out,
                 std::string* outError)
{
    Scene::SceneValue list;
    if (!Scene::ParseValue(value, list, outError))
        return false;
    if (list.Kind != Scene::SceneValueKind::Array)
    {
        if (outError)
            *outError = "expected a list of records";
        return false;
    }
    for (size_t index = 0; index < list.Items.size(); ++index)
    {
        AI::RecordStore::Record record;
        bool keep = true;
        if (!ReadRecord(ctx, list.Items[index], record, keep))
        {
            if (outError)
                *outError = "record " + std::to_string(index) +
                            " is not (subject, owner, kind, x, y, z, strength, value0, value1, firstSeen, lastSeen)";
            return false;
        }
        if (keep)
            out.push_back(record);
    }
    return true;
}

template <typename T>
bool ReadScalar(std::string_view value, T& out, std::string* outError)
{
    Scene::SceneValue parsed;
    if (!Scene::ParseValue(value, parsed, outError))
        return false;
    if constexpr (std::is_same_v<T, bool>)
    {
        if (parsed.Kind == Scene::SceneValueKind::Bool)
        {
            out = parsed.BoolValue;
            return true;
        }
    }
    else if (parsed.Kind == Scene::SceneValueKind::Int && parsed.IntValue >= 0 &&
             parsed.IntValue <= std::numeric_limits<T>::max())
    {
        out = static_cast<T>(parsed.IntValue);
        return true;
    }
    if (!outError)
        return false;
    if constexpr (std::is_same_v<T, bool>)
        *outError = "expected true or false";
    else
        *outError = "expected a whole number from 0 to " + std::to_string(std::numeric_limits<T>::max());
    return false;
}

// The entity's AiGroup, or the defaults when it has none yet.
Components::AiGroup ReadSettings(const ECS::World& world, ECS::EntityHandle entity)
{
    const Components::AiGroup* group = world.GetComponent<Components::AiGroup>(entity);
    return group != nullptr ? *group : Components::AiGroup{};
}

// Keeps the first `capacity` records, with one warning when the save has more.
void KeepRecordCapacity(uint32 capacity, std::vector<AI::RecordStore::Record>& records)
{
    if (records.size() <= capacity)
        return;
    Logger::Log::Warning("AiGroup scene load: the save has {} records, but RecordCapacity is {}, so the load keeps "
                         "the first {}. Raise AiGroup::RecordCapacity to keep them all.",
                         records.size(), capacity, capacity);
    records.resize(capacity);
}

// The scene schema of AiGroup: the settings, the plan cells and the memory records in the order a
// task reads them. With IdsAreEntities, a subject and an owner save as entity references, and a
// record whose subject entity is not in the save is dropped.
class AiGroupSceneSchema final : public Scene::ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "AiGroup"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, const Scene::SceneWriteContext& ctx,
                   std::vector<std::string>& outLines) const override
    {
        const Components::AiGroup* group = world.GetComponent<Components::AiGroup>(entity);
        if (group == nullptr)
            return;
        outLines.push_back("AiGroup.planCells = " + std::to_string(group->PlanCells));
        outLines.push_back("AiGroup.recordCapacity = " + std::to_string(group->RecordCapacity));
        outLines.push_back("AiGroup.forgetTicks = " + std::to_string(group->ForgetTicks));
        outLines.push_back(std::string("AiGroup.clearOnSight = ") + (group->ClearOnSight ? "true" : "false"));
        outLines.push_back(std::string("AiGroup.idsAreEntities = ") + (group->IdsAreEntities ? "true" : "false"));
        const AiGroupStore::Group* knowledge = FindKnowledge(world, entity, *group);
        if (knowledge == nullptr)
            return;
        outLines.push_back("AiGroup.plan = " + FormatAiValues(ctx, knowledge->Plan, {}));
        std::string records = "[";
        for (uint32 index = 0; index < knowledge->Records.GetCount(); ++index)
        {
            const std::string subject =
                FormatAiValue(ctx, knowledge->Records.GetSubjects()[index], group->IdsAreEntities);
            if (subject == Scene::FormatQuoted({}))
                continue;
            records += (records.size() > 1 ? ", " : "") +
                       FormatRecord(ctx, knowledge->Records, index, subject, group->IdsAreEntities);
        }
        outLines.push_back("AiGroup.records = " + records + "]");
    }

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity, const Scene::SceneReadContext& ctx,
                       std::string_view property, std::string_view value, std::string* outError) const override
    {
        // The save writes planCells and recordCapacity before the plan and the records.
        if (property == "plan")
        {
            std::vector<uint32> plan;
            if (!ReadAiValues(ctx, value, ReadSettings(world, entity).PlanCells, plan, outError))
                return false;
            FindOrMakeKnowledge(world, entity).Plan = std::move(plan);
            return true;
        }
        if (property == "records")
        {
            std::vector<AI::RecordStore::Record> records;
            if (!ReadRecords(ctx, value, records, outError))
                return false;
            KeepRecordCapacity(ReadSettings(world, entity).RecordCapacity, records);
            FindOrMakeKnowledge(world, entity).Records.Restore(records);
            return true;
        }
        Components::AiGroup group;
        if (const Components::AiGroup* existing = world.GetComponent<Components::AiGroup>(entity))
            group = *existing;
        bool read = true;
        if (property == "plancells")
            read = ReadScalar(value, group.PlanCells, outError);
        else if (property == "recordcapacity")
            read = ReadScalar(value, group.RecordCapacity, outError);
        else if (property == "forgetticks")
            read = ReadScalar(value, group.ForgetTicks, outError);
        else if (property == "clearonsight")
            read = ReadScalar(value, group.ClearOnSight, outError);
        else if (property == "idsareentities")
            read = ReadScalar(value, group.IdsAreEntities, outError);
        else
            return Scene::ReportUnknownField(ctx, GetComponentName(), property, outError);
        if (!read)
            return false;
        if (Components::AiGroup* existing = world.GetComponentForWrite<Components::AiGroup>(entity))
            *existing = group;
        else
            world.AddComponentImmediate(entity, group);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string*) const override
    {
        if (world.GetComponent<Components::AiGroup>(entity) == nullptr)
            world.AddComponentImmediate(entity, Components::AiGroup{});
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::AiGroup>(entity);
        return true;
    }
};

} // namespace

GE_REGISTER_SCENE_SCHEMA(AiGroupSceneSchema)

} // namespace GameEngine::AIECS
