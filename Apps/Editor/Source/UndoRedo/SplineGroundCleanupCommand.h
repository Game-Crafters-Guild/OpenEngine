#pragma once

#include "UndoRedo/IEditorCommand.h"

#include "SplineRecipes/SplineGroundCleanup.h"

namespace GameEngine::ECS { class World; }

namespace GameEngine::Editor
{

// The undo step of SplineRecipes::SplineGroundCleanup: removes the ground components a recipe
// made, and puts them back with their SplineGroundProvisioned marker on undo. A hand-authored
// component is never in the step.
class SplineGroundCleanupCommand final : public IEditorCommand
{
public:
    SplineGroundCleanupCommand(ECS::World* world, const SplineRecipes::SplineGroundCleanup& cleanup)
        : m_World(world)
        , m_Cleanup(cleanup)
    {
    }

    const char* GetName() const override { return "Remove Recipe Ground Effects"; }
    const char* GetTypeName() const override { return "SplineGroundCleanupCommand"; }

    void Do() override
    {
        if (m_World)
            m_Cleanup.Apply(*m_World);
    }

    void Undo() override
    {
        if (m_World)
            m_Cleanup.Revert(*m_World);
    }

private:
    ECS::World* m_World = nullptr;
    SplineRecipes::SplineGroundCleanup m_Cleanup;
};

} // namespace GameEngine::Editor
