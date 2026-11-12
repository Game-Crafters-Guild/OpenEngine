#pragma once

#include "AIECS/Components/AiGroup.h"
#include "ECS/Systems.h"

#include <vector>

namespace GameEngine::AIECS
{

class AiGroupStore;
class AiRegistry;

/// Once per AI tick, before the decisions: gives each new or copied group its knowledge in the
/// world's AiGroupStore, runs every registered sensor for each group, in entity order, then
/// forgets the group's records that are too old.
class AiKnowledgeSystem : public ECS::ISystem
{
public:
    static constexpr ECS::SystemCadence Cadence = ECS::SystemCadence::Simulation;

    explicit AiKnowledgeSystem(const AiRegistry& registry) : m_Registry(registry) {}

    void Update(ECS::World& world, float32 deltaTime) override;
    const char* GetName() const override { return "AiKnowledgeSystem"; }
    // A sensor takes World&, so no other system runs beside this one.
    bool RequiresExclusiveUpdate() const override { return true; }

private:
    struct GroupEntry
    {
        ECS::EntityHandle Entity;
        Components::AiGroup Group;
    };

    // Creates the entries the groups do not own yet and applies each group's record settings.
    void PrepareGroups(ECS::World& world, AiGroupStore& store);

    const AiRegistry& m_Registry;
    std::vector<GroupEntry> m_Entries;
};

} // namespace GameEngine::AIECS
