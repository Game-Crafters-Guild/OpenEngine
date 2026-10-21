#pragma once

#include "ECS/ECS.h"
#include "ECS/ModuleRegistration.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
namespace ECS
{
class World;
}
} // namespace GameEngine

namespace GameEngine::Editor
{

// An action a module offers on one mark-up: a button in the mark-up's inspector section and a
// row of the Mark-ups panel's context menu, so the editor core never names the module.
struct MarkupAction
{
    // Stable id and replace-forward key, e.g. "ai-assistant.ask".
    std::string Id;
    // The button's and the menu row's text. No '/': the context menu reads it as a submenu path.
    std::string Label;
    // An editor icon path (UI/EditorIcons.h) for the menu row; empty for none.
    std::string Icon;
    // Runs on the main thread when the user picks the action on `entity`, a mark-up of `world`.
    std::function<void(const ECS::World& world, ECS::EntityHandle entity)> Action;
    // Why the action cannot run on `entity` now, with the fix, shown on the disabled button and
    // menu row; empty when it can. Null: it can always run.
    std::function<std::string(const ECS::World& world, ECS::EntityHandle entity)> DisabledReason;
};

// The actions every mark-up offers, in registration order. Registration and reads are
// main-thread only, like the other editor registries; registering an Id again replaces the
// earlier action in place. An action without an Id, a Label or an Action, or whose Label holds a
// '/', is refused with an error that says so.
class MarkupActionRegistry
{
public:
    static MarkupActionRegistry& Get();

    MarkupActionRegistry(const MarkupActionRegistry&) = delete;
    MarkupActionRegistry& operator=(const MarkupActionRegistry&) = delete;

    void Register(MarkupAction action);

    std::vector<const MarkupAction*> GetActions() const;

    // C12 editor-kind unload-refusal diagnostics: appends a description of every action
    // registered while `moduleId`'s registrars ran (ECS/ModuleRegistration.h).
    void AppendModulePins(std::string_view moduleId, std::vector<std::string>& outPins) const;

private:
    MarkupActionRegistry() = default;

    struct Entry
    {
        MarkupAction Action;
        ECS::ModuleRegistrationStamp Module;
    };

    std::vector<Entry> m_Actions; // registration order
};

} // namespace GameEngine::Editor
