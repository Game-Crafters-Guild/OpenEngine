#include "AIECS/AiRegistry.h"
#include "AIECS/Components/AiAgent.h"
#include "AIECS/Components/AiCells/AiCellsLarge.h"
#include "AIECS/Components/AiCells/AiCellsMedium.h"
#include "AIECS/Components/AiCells/AiCellsSmall.h"
#include "ECS/ECSTemplates.h"
#include "Scene/AiSceneText.h"
#include "Scene/SceneSchemaRegistry.h"

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::AIECS
{
namespace
{

// The cells of the agent's program that hold entities; empty when the entity has no program.
std::span<const uint16> FindEntityCells(const ECS::World& world, ECS::EntityHandle entity)
{
    const Components::AiAgent* agent = world.GetComponent<Components::AiAgent>(entity);
    const AiProgram* program = agent != nullptr ? AiRegistry::Get().FindProgram(agent->Program) : nullptr;
    return program != nullptr ? std::span<const uint16>(program->EntityCells) : std::span<const uint16>();
}

// The scene schema of one cell class: "<Class>.values = [...]". The cells that the agent's program
// declares in AiProgram::EntityCells save as entity references; the other cells save as numbers.
template <typename Cells>
class AiCellsSceneSchema : public Scene::ISceneComponentSchema
{
  public:
    void Serialize(const ECS::World& world, ECS::EntityHandle entity, const Scene::SceneWriteContext& ctx,
                   std::vector<std::string>& outLines) const override
    {
        const Cells* cells = world.GetComponent<Cells>(entity);
        if (cells == nullptr)
            return;
        outLines.push_back(std::string(GetComponentName()) + ".values = " +
                           FormatAiValues(ctx, cells->Values, FindEntityCells(world, entity)));
    }

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity, const Scene::SceneReadContext& ctx,
                       std::string_view property, std::string_view value, std::string* outError) const override
    {
        if (property != "values")
            return Scene::ReportUnknownField(ctx, GetComponentName(), property, outError);
        std::vector<uint32> values;
        if (!ReadAiValues(ctx, value, Cells::kCount, values, outError))
            return false;
        Cells cells{};
        std::copy(values.begin(), values.end(), cells.Values.begin());
        if (Cells* existing = world.GetComponentForWrite<Cells>(entity))
            *existing = cells;
        else
            world.AddComponentImmediate(entity, cells);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string*) const override
    {
        if (world.GetComponent<Cells>(entity) == nullptr)
            world.AddComponentImmediate(entity, Cells{});
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Cells>(entity);
        return true;
    }
};

class AiCellsSmallSceneSchema final : public AiCellsSceneSchema<Components::AiCellsSmall>
{
  public:
    std::string_view GetComponentName() const override { return "AiCellsSmall"; }
};

class AiCellsMediumSceneSchema final : public AiCellsSceneSchema<Components::AiCellsMedium>
{
  public:
    std::string_view GetComponentName() const override { return "AiCellsMedium"; }
};

class AiCellsLargeSceneSchema final : public AiCellsSceneSchema<Components::AiCellsLarge>
{
  public:
    std::string_view GetComponentName() const override { return "AiCellsLarge"; }
};

} // namespace

GE_REGISTER_SCENE_SCHEMA(AiCellsSmallSceneSchema)
GE_REGISTER_SCENE_SCHEMA(AiCellsMediumSceneSchema)
GE_REGISTER_SCENE_SCHEMA(AiCellsLargeSceneSchema)

} // namespace GameEngine::AIECS
