#pragma once

#include "AiDecisionRow.h"
#include "ECS/Systems.h"

#include <memory>
#include <vector>

namespace GameEngine::AIECS
{

class AiRegistry;

/// Carries out the orders of this AI tick's decisions through the registered command functions,
/// on the main thread, in entity order, so the result does not depend on the job split.
class AiApplySystem : public ECS::ISystem
{
public:
    static constexpr ECS::SystemCadence Cadence = ECS::SystemCadence::Simulation;

    AiApplySystem(const AiRegistry& registry, std::shared_ptr<std::vector<AiDecisionRow>> rows);

    void Update(ECS::World& world, float32 deltaTime) override;
    const char* GetName() const override { return "AiApplySystem"; }
    bool RequiresExclusiveUpdate() const override { return true; }

private:
    // Logs one error for each command type that a task issued and that is not registered.
    void ReportUnregisteredCommand(uint16 type);

    const AiRegistry& m_Registry;
    std::shared_ptr<std::vector<AiDecisionRow>> m_Rows;
    std::vector<uint16> m_ReportedCommands;
};

} // namespace GameEngine::AIECS
