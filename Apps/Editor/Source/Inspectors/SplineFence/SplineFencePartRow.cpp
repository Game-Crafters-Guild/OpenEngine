#include "Inspectors/SplineFence/SplineFencePartRow.h"

#include "Inspectors/DefaultComponentInspector.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Logger/Logger.h"
#include "UI/AssetField.h"
#include "UI/ControlLayoutLoader.h"
#include "UI/Controls/Dropdown.h"

#include "Assets/AssetRegistry.h"
#include "AssetCore/GUID.h"
#include "Components/Spline/SplineFence.h"
#include "Core/Engine.h"
#include "ECS/ComponentFieldRegistry.h"
#include "ECS/ECSTemplates.h"
#include "Types/ParseNumber.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine::EditorUI
{
namespace
{

using Components::SplineFencePart;
using Components::SplineFencePartKind;

constexpr const char* kLayoutAssetPath = "UI/inspectors/SplineFencePartRow/SplineFencePartRow.uxml";
constexpr const char* kStyleAssetPath = "UI/inspectors/SplineFencePartRow/SplineFencePartRow.css";

constexpr const char* kMeshTooltip = "The mesh of this part. A part without a mesh places nothing.";

// The reflected Kind field of a fence part: its enumerators name the dropdown's options and its
// tooltip explains them.
const ECS::FieldInfo* KindField()
{
    return ECS::ComponentFieldRegistry::FindField(ECS::GetComponentTypeId<SplineFencePart>(), "Kind");
}

// Every kind except None, which is an empty slot and not a kind a part can take.
std::vector<Dropdown::Option> KindOptions(const ECS::FieldInfo& field)
{
    std::vector<Dropdown::Option> options;
    for (const ECS::EnumNameValue& value : field.EnumNames)
    {
        if (value.Value != static_cast<int64_t>(SplineFencePartKind::None))
            options.push_back({std::to_string(value.Value), EnumDisplayName(value)});
    }
    return options;
}

int KindIndex(const std::vector<Dropdown::Option>& options, SplineFencePartKind kind)
{
    const std::string value = std::to_string(static_cast<int>(kind));
    for (std::size_t i = 0; i < options.size(); ++i)
    {
        if (options[i].value == value)
            return static_cast<int>(i);
    }
    return 0;
}

std::optional<SplineFencePartKind> KindOfOption(const std::vector<Dropdown::Option>& options,
                                                const std::string& value)
{
    const std::optional<int64_t> parsed = ParseInt(value);
    const bool offered = std::any_of(options.begin(), options.end(),
                                     [&value](const Dropdown::Option& option) { return option.value == value; });
    if (!parsed || !offered)
        return std::nullopt;
    return static_cast<SplineFencePartKind>(*parsed);
}

void WireKind(Dropdown& kind, SplineFencePartKind current, std::function<void(SplineFencePartKind)> onChanged)
{
    const ECS::FieldInfo* field = KindField();
    if (!field || field->EnumNames.empty())
    {
        Logger::Log::Error("SplineFencePartRow: SplineFencePart::Kind has no reflected enumerators, so the "
                           "part row cannot offer its kinds. Rebuild the component reflection.");
        return;
    }
    std::vector<Dropdown::Option> options = KindOptions(*field);
    kind.SetOptions(options, KindIndex(options, current));
    if (!field->Tooltip.empty())
        kind.SetTooltip(std::string(field->Tooltip));
    kind.SetOnValueChanged(
        [options = std::move(options), onChanged = std::move(onChanged)](const std::string& value)
        {
            const std::optional<SplineFencePartKind> chosen = KindOfOption(options, value);
            if (chosen && onChanged)
                onChanged(*chosen);
        });
}

void WireMesh(AssetField& mesh, const GUID& current, IThumbnailProvider* thumbnails,
              std::function<void(const GUID&)> onChanged)
{
    mesh.SetAcceptedTypes({AssetType::Model});
    mesh.SetAssetRegistry(&EngineCore::GetInstance().GetAssetManager().GetRegistry());
    if (thumbnails)
        mesh.SetThumbnailProvider(thumbnails);
    mesh.SetValue(current);
    mesh.SetTooltip(kMeshTooltip);
    mesh.SetOnValueChanged(std::move(onChanged));
}

} // namespace

SplineFencePartRow::SplineFencePartRow(const SplineFencePart& part, bool first, bool last,
                                       IThumbnailProvider* thumbnails, Handlers handlers)
{
    AddClass("spline-fence-part");
    RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");

    std::unique_ptr<UIElement> row = Editor::ControlLayoutLoader::Load(kLayoutAssetPath, {});
    if (!row)
        return;
    UIElement* kindCell = InspectorDrag::FindChildByClass(row.get(), "spline-fence-part-kind-cell");
    UIElement* fieldCell = InspectorDrag::FindChildByClass(row.get(), "spline-fence-part-field");
    auto* kind = dynamic_cast<Dropdown*>(InspectorDrag::FindChildByClass(kindCell, "spline-fence-part-kind"));
    UIElement* meshCell = InspectorDrag::FindChildByClass(fieldCell, "spline-fence-part-mesh-cell");
    auto* mesh = dynamic_cast<AssetField*>(InspectorDrag::FindChildByClass(meshCell, "spline-fence-part-mesh"));
    auto* actions =
        dynamic_cast<ListItemActions*>(InspectorDrag::FindChildByClass(fieldCell, "spline-fence-part-actions"));
    if (!kind || !mesh || !actions)
    {
        Logger::Log::Error("SplineFencePartRow: {} needs the kind dropdown, the mesh field and the list actions.",
                           kLayoutAssetPath);
        return;
    }
    WireKind(*kind, part.Kind, std::move(handlers.KindChanged));
    WireMesh(*mesh, part.Mesh.ToGuid(), thumbnails, std::move(handlers.MeshChanged));
    actions->SetHandlers(std::move(handlers.Actions), first, last);
    AddChild(std::move(row));
}

} // namespace GameEngine::EditorUI
