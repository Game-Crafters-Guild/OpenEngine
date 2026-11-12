#include "Inspectors/InspectorOverrideRows.h"

#include "InspectorRegistry.h"
#include "Inspectors/InspectorComponent/InspectorComponentRowHelpers.h" // GetAdditionalEntities
#include "Inspectors/InspectorUIHelpers.h"

#include "ECS/Entity.h" // ECS::World
#include "UI/UIElement.h"

#include <string_view>
#include <utility>

namespace GameEngine
{
namespace
{

// The menu items of the entries the override answerer gives `target`, each followed by `refresh`.
std::vector<ContextMenuManipulator::Item> OverrideMenuItems(const OverrideMenuTarget& target,
                                                            const std::function<void()>& refresh)
{
    std::vector<ContextMenuManipulator::Item> items;
    if (!target.World || !target.World->IsValid(target.Entity))
        return items;
    for (OverrideMenuEntry& entry : InspectorRegistry::Get().QueryOverrideMenu(target))
    {
        items.push_back({.Path = std::move(entry.Label),
                         .IconPath = std::move(entry.IconPath),
                         .OnActivate = [run = std::move(entry.Run), refresh]()
                         {
                             run();
                             if (refresh)
                                 refresh();
                         }});
    }
    return items;
}

// The menu of a bound row: the override entries, a separator when both sets have entries, then the row's own.
std::vector<ContextMenuManipulator::Item> FieldRowMenuItems(const OverrideMenuTarget& target,
                                                            const std::function<void()>& refresh,
                                                            const std::vector<ContextMenuManipulator::Item>& rowItems)
{
    std::vector<ContextMenuManipulator::Item> items = OverrideMenuItems(target, refresh);
    if (!items.empty() && !rowItems.empty())
        items.push_back({.Separator = true});
    items.insert(items.end(), rowItems.begin(), rowItems.end());
    return items;
}

void SetFieldRowMark(ECS::World& world, ECS::EntityHandle entity, ECS::ComponentTypeId typeId, UIElement& row,
                     std::string_view field)
{
    const ComponentOverrides overrides = InspectorRegistry::Get().QueryComponentOverrides(world, entity, typeId);
    InspectorUI::SetRowOverridden(&row, overrides.IsFieldOverridden(field));
}

} // namespace

void BindFieldRow(const InspectorContext& context, ECS::ComponentTypeId typeId, UIElement* row, std::string field,
                  std::vector<ContextMenuManipulator::Item> rowItems)
{
    if (!row)
        return;
    // The override marks follow the source of one entity.
    const bool singleSelection = context.World && InspectorDrag::GetAdditionalEntities(context).empty();
    if (!singleSelection && rowItems.empty())
        return;
    OverrideMenuTarget target;
    if (singleSelection)
    {
        SetFieldRowMark(*context.World, context.Entity, typeId, *row, field);
        if (context.OverrideMarkRefreshCallbacks)
            context.OverrideMarkRefreshCallbacks->push_back(
                [world = context.World, entity = context.Entity, typeId, row, field]()
                { SetFieldRowMark(*world, entity, typeId, *row, field); });
        const UIElement* label = InspectorUI::RowLabel(row);
        target = {.World = context.World,
                  .Entity = context.Entity,
                  .Component = typeId,
                  .Field = std::move(field),
                  .Undo = context.Undo,
                  .Notifications = context.ChangeNotifications,
                  .Label = label ? label->GetTextContent() : std::string()};
    }
    row->AddManipulator(ContextMenuManipulator::Create(
        [target = std::move(target), refresh = context.RequestInspectorRefresh, rowItems = std::move(rowItems)]()
        { return FieldRowMenuItems(target, refresh, rowItems); }));
}

void AttachOverrideMenu(UIElement& element, const OverrideMenuTarget& target, std::function<void()> refresh)
{
    element.AddManipulator(ContextMenuManipulator::Create([target, refresh = std::move(refresh)]()
                                                          { return OverrideMenuItems(target, refresh); }));
}

} // namespace GameEngine
