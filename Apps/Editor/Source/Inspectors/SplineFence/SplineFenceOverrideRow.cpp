#include "Inspectors/SplineFence/SplineFenceOverrideRow.h"

#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Inspectors/SplineFence/SplineFenceOverrideFields.h"
#include "Logger/Logger.h"
#include "SplineRecipes/FenceSpanOverrides.h"
#include "UI/ControlLayoutLoader.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/InspectorNotice.h"
#include "UI/Controls/IntField.h"

#include "Components/Spline/SplineFence.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine::EditorUI
{
namespace
{

using Components::SplineSpanOverride;
using Components::SplineSpanOverrideKind;

constexpr const char* kLayoutAssetPath = "UI/inspectors/SplineFenceOverrideRow/SplineFenceOverrideRow.uxml";
constexpr const char* kStyleAssetPath = "UI/inspectors/SplineFenceOverrideRow/SplineFenceOverrideRow.css";

constexpr const char* kKindTooltip = "Gate draws a Gate part in the span and clears the crest parts over it. "
                                     "Explicit Part draws one Span part there instead of the part the Seed picks.";
constexpr const char* kPartTooltip = "The part the span draws, from the fence's Parts list.";
constexpr const char* kPointTooltip = "The point of the spline that opens the segment, counted from 0.";
constexpr const char* kSpanTooltip = "Which span of that segment, counted from 0 at the point.";

// The fields the layout must hold. Each is null when the layout does not hold it.
struct RowFields
{
    Dropdown* Kind = nullptr;
    Dropdown* Part = nullptr;
    ListItemActions* Actions = nullptr;
    IntField* Point = nullptr;
    IntField* Span = nullptr;
};

// The layout's fields, found level by level from the body.
RowFields FindFields(UIElement& body)
{
    UIElement* head = InspectorDrag::FindChildByClass(&body, "spline-fence-override-head");
    UIElement* kindCell = InspectorDrag::FindChildByClass(head, "spline-fence-override-kind-cell");
    UIElement* field = InspectorDrag::FindChildByClass(head, "spline-fence-override-field");
    UIElement* place = InspectorDrag::FindChildByClass(&body, "spline-fence-override-place");
    RowFields fields;
    fields.Kind = dynamic_cast<Dropdown*>(InspectorDrag::FindChildByClass(kindCell, "spline-fence-override-kind"));
    fields.Part = dynamic_cast<Dropdown*>(InspectorDrag::FindChildByClass(field, "spline-fence-override-part"));
    fields.Actions =
        dynamic_cast<ListItemActions*>(InspectorDrag::FindChildByClass(field, "spline-fence-override-actions"));
    fields.Point = dynamic_cast<IntField*>(InspectorDrag::FindChildByClass(place, "spline-fence-override-point"));
    fields.Span = dynamic_cast<IntField*>(InspectorDrag::FindChildByClass(place, "spline-fence-override-span"));
    return fields;
}

// Gate and Explicit Part: a row removes its override with Remove, so None is not offered.
void WireKind(Dropdown& kind, SplineSpanOverrideKind current, std::function<void(SplineSpanOverrideKind)> onChanged)
{
    std::vector<Dropdown::Option> options = Editor::SpanOverrideKindOptions({});
    kind.SetOptions(options, Editor::SpanOverrideKindIndex(options, current));
    kind.SetTooltip(kKindTooltip);
    kind.SetOnValueChanged(
        [options = std::move(options), onChanged = std::move(onChanged)](const std::string& value)
        {
            const std::optional<SplineSpanOverrideKind> chosen = Editor::SpanOverrideKindOfOption(options, value);
            if (chosen && onChanged)
                onChanged(*chosen);
        });
}

void WirePart(Dropdown& part, const Components::SplineFence& fence, const SplineSpanOverride& entry,
              std::function<void(uint8_t)> onChanged)
{
    const Editor::OverridePartChoices choices =
        Editor::OverridePartOptions(fence, SplineRecipes::SpanOverridePartKind(entry.Kind), entry.PoolSlot);
    part.SetOptions(choices.Options, choices.Selected);
    part.SetTooltip(kPartTooltip);
    part.SetOnValueChanged(
        [onChanged = std::move(onChanged)](const std::string& value)
        {
            const std::optional<uint8_t> picked = Editor::OverridePartOfOption(value);
            if (picked && onChanged)
                onChanged(*picked);
        });
}

// A number field that commits a value in [0, `max`].
void WireNumber(IntField& field, int value, int max, const char* tooltip, std::function<void(int)> onChanged)
{
    field.SetRange(0, max);
    field.SetValueWithoutNotify(value);
    field.SetTooltip(tooltip);
    field.SetOnValueChanged(
        [onChanged = std::move(onChanged)](const int& committed)
        {
            if (onChanged)
                onChanged(committed);
        });
}

} // namespace

SplineFenceOverrideRow::SplineFenceOverrideRow(const Components::SplineFence& fence, uint32_t slot, bool first,
                                               bool last, const std::string& problem, Handlers handlers)
{
    AddClass("spline-fence-override-row");
    RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");

    if (slot >= Components::kSplineFenceMaxSpanOverrides)
    {
        Logger::Log::Error("SplineFenceOverrideRow: slot {} is past the last span override, so it has no row.", slot);
        return;
    }
    std::unique_ptr<UIElement> body = Editor::ControlLayoutLoader::Load(kLayoutAssetPath, {});
    if (!body)
        return;
    const RowFields fields = FindFields(*body);
    if (!fields.Kind || !fields.Part || !fields.Actions || !fields.Point || !fields.Span)
    {
        Logger::Log::Error("SplineFenceOverrideRow: {} needs the kind and part dropdowns, the list actions and the "
                           "point and span fields.",
                           kLayoutAssetPath);
        return;
    }
    const SplineSpanOverride& entry = fence.Overrides[slot];
    WireKind(*fields.Kind, entry.Kind, std::move(handlers.KindChanged));
    WirePart(*fields.Part, fence, entry, std::move(handlers.PartChanged));
    fields.Actions->SetHandlers(std::move(handlers.Actions), first, last);
    WireNumber(*fields.Point, static_cast<int>(std::min<uint32_t>(entry.PointIndex, std::numeric_limits<int>::max())),
               std::numeric_limits<int>::max(), kPointTooltip,
               [onChanged = std::move(handlers.PointChanged)](int value)
               {
                   if (onChanged)
                       onChanged(static_cast<uint32_t>(value));
               });
    WireNumber(*fields.Span, entry.SpanOrdinal, std::numeric_limits<uint16_t>::max(), kSpanTooltip,
               [onChanged = std::move(handlers.SpanChanged)](int value)
               {
                   if (onChanged)
                       onChanged(static_cast<uint16_t>(value));
               });
    if (!problem.empty())
        InspectorUI::AddNotice(body.get(), problem, InspectorNotice::Kind::Warning);
    AddChild(std::move(body));
}

} // namespace GameEngine::EditorUI
