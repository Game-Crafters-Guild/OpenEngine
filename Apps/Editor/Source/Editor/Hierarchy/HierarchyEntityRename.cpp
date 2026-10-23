#include "Editor/Hierarchy/HierarchyEntityRename.h"
#include "Editor/Hierarchy/EntityNameEdit.h"

#include "EditorChangeNotifications.h"
#include "UndoRedo/EntityComponentsEdit.h"

#include "Components/Name.h"
#include "UI/Controls/Label.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace GameEngine::Editor
{

namespace
{

constexpr const char* kRenameEntityLabel = "Rename Entity";
// Name::value holds the name's UTF-8 bytes and a terminating null.
constexpr std::size_t kMaxEntityNameBytes = sizeof(Components::Name::value) - 1;

} // namespace

std::string ValidateEntityName(const std::string& name)
{
    const bool blank = std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isspace(c) != 0; });
    if (blank)
        return "Name can't be empty";
    if (name.size() > kMaxEntityNameBytes)
        return "Name is too long: " + std::to_string(kMaxEntityNameBytes) +
               " characters at most, fewer with accented or non-Latin letters";
    const bool control = std::any_of(name.begin(), name.end(), [](unsigned char c) { return std::iscntrl(c) != 0; });
    if (control)
        return "Name can't contain control characters";
    return {};
}

bool CommitEntityRename(ECS::World& world, ECS::EntityHandle entity, const std::string& name, UndoRedoService* undo,
                        EditorChangeNotifications* notifications)
{
    if (!ValidateEntityName(name).empty() || !entity.IsValid() || !world.IsValid(entity))
        return false;

    const bool renamed = CommitEntityComponentsEdit(world, entity, undo, notifications, kRenameEntityLabel, [&]()
    {
        Components::Name value{};
        std::memcpy(value.value, name.data(), name.size());
        if (Components::Name* existing = world.GetComponentForWrite<Components::Name>(entity))
            *existing = value;
        else
            world.AddComponentImmediate(entity, value);
        return true;
    });
    if (renamed && notifications)
    {
        notifications->NotifyComponentChanged(
            {&world, entity, ECS::GetComponentTypeId<Components::Name>(), EditorChangeNotifications::ChangeKind::Commit});
    }
    return renamed;
}

void HierarchyEntityRename::Begin(Label* title, UIElement* tree, ECS::World& world, ECS::EntityHandle entity,
                                  UndoRedoService* undo, EditorChangeNotifications* notifications)
{
    if (!entity.IsValid() || !world.IsValid(entity))
        return;
    std::string current;
    if (const auto* name = world.GetComponent<Components::Name>(entity))
        current = std::string(name->View());

    m_Entity = entity;
    m_Session.Begin(title, tree, current, "tree-rename-field", &ValidateEntityName,
                    [&world, entity, undo, notifications](const std::string& name)
                    { (void)CommitEntityRename(world, entity, name, undo, notifications); });
}

void HierarchyEntityRename::OnRowBound(const UIElement& row, ECS::EntityHandle shown)
{
    m_Session.OnRowBound(row, shown == m_Entity);
}

} // namespace GameEngine::Editor
