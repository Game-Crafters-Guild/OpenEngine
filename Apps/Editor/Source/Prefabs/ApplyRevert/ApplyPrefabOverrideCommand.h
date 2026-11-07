#pragma once

#include "UndoRedo/EntityComponentsEdit.h"
#include "UndoRedo/IEditorCommand.h"

#include "ECS/ECS.h"
#include "Scene/PrefabAuthoring.h"

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{
class EditorChangeNotifications;

/// The undo step of an Apply to prefab: overrides of prefab entities written into the prefab files that author
/// the entities, and the change given to every instance of the world. Undo writes each file's text from before
/// and gives each applying entity its components from before; Redo writes the text from after.
class ApplyPrefabOverrideCommand final : public IEditorCommand
{
    // Only Create can name this type, so only Create constructs a command.
    struct ConstructionKey
    {
        explicit ConstructionKey() = default;
    };

  public:
    /// Applies `targets` with one write per prefab file and names the step `name`. Null, with `outError` stating
    /// the cause, when nothing was written. A command with `outError` set covers the files written before the
    /// write that failed.
    /// The caller publishes the initial change through CommitAlreadyApplied; the command publishes replay.
    static std::unique_ptr<ApplyPrefabOverrideCommand> Create(ECS::World& world, EditorChangeNotifications* notifications,
                                                              std::span<const Scene::PrefabOverrideTarget> targets,
                                                              std::string name, std::string& outError);

    const char* GetName() const override { return m_Name.c_str(); }
    const char* GetTypeName() const override { return "ApplyPrefabOverrideCommand"; }
    void Do() override {}
    void Undo() override;
    void Redo() override;

    ApplyPrefabOverrideCommand(ConstructionKey key, ECS::World& world, EditorChangeNotifications* notifications,
                               std::string name);

  private:
    // An entity whose overrides the step applies, with the component of each override and its components from before.
    struct Applier
    {
        ECS::EntityHandle Entity;
        std::vector<ECS::ComponentTypeId> Components;
        EntityComponentBytes Before;
    };

    void WriteText(const Scene::PrefabFileEdit& edit, const std::string& text);

    std::string m_Name;
    ECS::World& m_World;
    EditorChangeNotifications* m_Notifications = nullptr;
    std::vector<Scene::PrefabFileEdit> m_Edits;
    std::vector<Applier> m_Appliers;
};

} // namespace GameEngine::Editor
