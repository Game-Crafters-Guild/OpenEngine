#include "Inspectors/ExposureCompensationCurveRow.h"

#include "Components/Rendering/Camera.h"
#include "Core/Engine.h"
#include "ECS/World.h"
#include "EditorContextMenu/UIContextMenu.h"
#include "Engine/Rendering/IncidentLightMeter.h"
#include "Engine/Rendering/RenderServices.h"
#include "InspectorRegistry.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Mathematics/Curve.h"
#include "UI/Controls/CurveField.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/Label.h"
#include "UI/EditorIcons.h"
#include "UI/StyleProperties.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine
{
namespace
{
using Keys = std::vector<Math::CurveKey>;

// The graph's axes: metered EV100 from below a moonless night to above a bright noon (widened to the
// camera's Auto Min and Max EV and to every key), compensation in stops.
constexpr float kAxisMinEv = -14.0f;
constexpr float kAxisMaxEv = 18.0f;
constexpr float kMinStops = -6.0f;
constexpr float kMaxStops = 2.0f;
constexpr uint32_t kDeleteKeyCommand = 1;

constexpr const char* kRowTooltip =
    "Auto exposure: stops added to the exposure at each scene brightness the meter reads (EV, left "
    "dark, right bright). The default leaves day, dusk and lamp-lit interiors as metered and darkens a "
    "moonlit night so it stays dark. With the Lights meter, the line marks the light on the scene now. "
    "Drag a key; right-click empty space to add one, a key to delete it.";

struct EvAxis
{
    float Min = kAxisMinEv;
    float Max = kAxisMaxEv;
};

EvAxis AxisFor(const Components::Camera& camera)
{
    const Math::Curve& curve = camera.AutoExposureCompensationCurve;
    EvAxis axis{std::min(kAxisMinEv, camera.AutoExposureMinEv), std::max(kAxisMaxEv, camera.AutoExposureMaxEv)};
    if (curve.KeyCount > 0)
    {
        axis.Min = std::min(axis.Min, curve.Keys[0].Time);
        axis.Max = std::max(axis.Max, curve.Keys[curve.KeyCount - 1].Time);
    }
    return axis;
}

std::string FormatStops(float stops)
{
    char text[16];
    std::snprintf(text, sizeof(text), stops > 0.0f ? "%+.0f" : "%.0f", stops);
    return text;
}

std::string FormatEv(float ev)
{
    char text[16];
    std::snprintf(text, sizeof(text), "%.0f", ev);
    return text;
}

Keys CurveKeys(const Math::Curve& curve)
{
    return Keys(curve.Keys, curve.Keys + curve.KeyCount);
}

void StoreCurveKeys(Components::Camera& camera, const Keys& keys)
{
    Math::Curve curve{};
    for (const Math::CurveKey& key : keys)
        curve.TryInsert(key);
    camera.AutoExposureCompensationCurve = curve;
}

std::unique_ptr<CurveField> MakeCompensationGraph(const Components::Camera& camera, const EvAxis& axis)
{
    auto graph = std::make_unique<CurveField>();
    CurveField::Config config;
    config.TimeMin = axis.Min;
    config.TimeMax = axis.Max;
    config.ValueMin = kMinStops;
    config.ValueMax = kMaxStops;
    config.AllowTimeDrag = true;
    config.AllowAddRemove = true;
    config.ValueAxisMarks = {0.0f, -2.0f, -4.0f};
    config.ValueLabel = &FormatStops;
    config.TimeAxisLabels = CurveField::TimeLabels::Formatted;
    config.TimeLabel = &FormatEv;
    config.ShowPlaybackIndicator = true;
    config.ShowPlaybackValueMarker = false;
    config.MinKeys = 2;
    config.MaxKeys = Math::Curve::Capacity;
    graph->SetConfig(config);
    graph->SetKeys(CurveKeys(camera.AutoExposureCompensationCurve));
    graph->SetPlaybackIndicator(0.0f, 0.0f, false);
    graph->SetShadedSpans({{axis.Min, -1.0f, "Night"}, {10.0f, axis.Max, "Day"}});
    graph->Overrides()
        .Set(Style::FlexGrow, 1.0f)
        .Set(Style::MinWidth, StyleLength::Px(160.0f));
    return graph;
}

// The light on the camera's world now, as the Lights meter reads it; empty without a renderer or
// before the environment's first reading.
std::optional<float> WorldLightsReadingEv(ECS::World& world)
{
    auto* services = EngineCore::GetInstance().GetRenderServices();
    if (!services)
        return std::nullopt;
    const std::optional<Engine::Renderer::IncidentLight> light = Engine::Renderer::MeasureIncidentLight(
        services->GetWorldLights(world.GetWorldId()), Engine::Renderer::ReadIncidentEnvironment(*services));
    if (!light)
        return std::nullopt;
    return Engine::Renderer::IncidentLightEv100(*light);
}

// The line marks the Lights meter's reading, on a camera that meters with it. A Picture-metered camera
// shows no line: its reading is the rendered image's, which the inspector does not read back.
void UpdateReadingMarker(CurveField& graph, ECS::World& world, ECS::EntityHandle entity)
{
    const auto* camera = world.GetComponent<Components::Camera>(entity);
    const std::optional<float> ev = camera && camera->AutoExposureMeter == Components::ExposureMeter::Lights
                                        ? WorldLightsReadingEv(world)
                                        : std::nullopt;
    if (!ev)
    {
        graph.SetPlaybackIndicator(0.0f, 0.0f, false);
        return;
    }
    const Keys& keys = graph.GetKeys();
    const float stops = Math::EvaluateCurveKeys(keys.data(), static_cast<uint32_t>(keys.size()), *ev);
    graph.SetPlaybackIndicator(*ev, stops, true);
}

void SyncKeyFields(const CurveField& graph, FloatField& evField, FloatField& stopsField)
{
    const int selected = graph.GetSelectedKey();
    const Keys& keys = graph.GetKeys();
    if (selected < 0 || selected >= static_cast<int>(keys.size()))
        return;
    evField.SetValueWithoutNotify(keys[static_cast<size_t>(selected)].Time);
    stopsField.SetValueWithoutNotify(keys[static_cast<size_t>(selected)].Value);
}

void ShowKeyContextMenu(CurveField* graph, Platform::Window* window, int keyIndex, float x, float y)
{
    if (!graph || !window)
        return;
    static std::shared_ptr<INativeContextMenu> s_Menu;
    if (!s_Menu)
        s_Menu = CreateContextMenu();
    if (!s_Menu)
        return;
    const bool canDelete = graph->GetKeys().size() > 2;
    s_Menu->Clear();
    s_Menu->SetCommandHandler([graph, keyIndex](uint32_t command)
    {
        if (command == kDeleteKeyCommand)
            graph->DeleteKey(keyIndex);
    });
    s_Menu->AddItem(0, "Delete Key", kDeleteKeyCommand, canDelete ? MenuItemFlag_None : MenuItemFlag_Disabled);
    s_Menu->SetItemIcon(kDeleteKeyCommand, EditorIcons::kTrash);
    s_Menu->Show(window, static_cast<int>(x), static_cast<int>(y));
}
} // namespace

CurveField* AddExposureCompensationCurveRow(const InspectorContext& ctx)
{
    if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
        return nullptr;
    const auto* camera = ctx.World->GetComponent<Components::Camera>(ctx.Entity);
    if (!camera)
        return nullptr;

    UIElement* row = InspectorUI::AddRow(ctx.Parent);
    row->Overrides().Set(Style::AlignItems, AlignItems::FlexStart);
    if (Label* label = InspectorUI::AddLabel(row, "Compensation Curve", kRowTooltip))
        label->AddClass("inspector-label-no-drag");
    UIElement* field = InspectorUI::AddFieldContainer(row);
    field->Overrides()
        .Set(Style::FlexDir, FlexDirection::Column)
        .Set(Style::AlignItems, AlignItems::Stretch)
        .Set(Style::Gap, StyleLength::Px(4.0f));

    const EvAxis axis = AxisFor(*camera);
    std::unique_ptr<CurveField> ownedGraph = MakeCompensationGraph(*camera, axis);
    CurveField* graph = ownedGraph.get();
    field->AddChild(std::move(ownedGraph));

    auto handlers = InspectorDrag::MakeComponentInteractiveHandlers<Components::Camera, Keys>(
        ctx.World, ctx.Entity, ctx.ChangeNotifications, ctx.Undo, "Edit Compensation Curve",
        [](Components::Camera& c, Keys keys) { StoreCurveKeys(c, keys); },
        InspectorDrag::GetAdditionalEntities(ctx));
    const std::function<void(Keys)> preview = std::move(handlers.first);
    const std::function<void(Keys)> commit = std::move(handlers.second);

    FloatField* evField = InspectorDrag::AddFloatRowWithDrag(
        field, "Key EV", 0.0f,
        [graph, preview](float ev)
        {
            if (graph->SetSelectedKeyTime(ev) >= 0)
                preview(graph->GetKeys());
        },
        [graph, commit](float ev)
        {
            if (graph->SetSelectedKeyTime(ev) >= 0)
                commit(graph->GetKeys());
        },
        std::numeric_limits<float>::quiet_NaN(), "The selected key's metered scene EV.", axis.Min, axis.Max);
    FloatField* stopsField = InspectorDrag::AddFloatRowWithDrag(
        field, "Key Stops", 0.0f,
        [graph, preview](float stops)
        {
            if (graph->SetSelectedKeyValue(stops) >= 0)
                preview(graph->GetKeys());
        },
        [graph, commit](float stops)
        {
            if (graph->SetSelectedKeyValue(stops) >= 0)
                commit(graph->GetKeys());
        },
        std::numeric_limits<float>::quiet_NaN(), "The selected key's compensation in stops (+ brightens).",
        kMinStops, kMaxStops);

    graph->SetOnChanging([graph, preview, evField, stopsField](const Keys& keys)
    {
        preview(keys);
        SyncKeyFields(*graph, *evField, *stopsField);
    });
    graph->SetOnChanged([graph, commit, evField, stopsField](const Keys& keys)
    {
        commit(keys);
        SyncKeyFields(*graph, *evField, *stopsField);
    });
    graph->SetOnSelectionChanged([graph, evField, stopsField](int) { SyncKeyFields(*graph, *evField, *stopsField); });
    graph->SetOnKeyContextMenu([graph, window = ctx.Window](int keyIndex, float x, float y)
    {
        ShowKeyContextMenu(graph, window, keyIndex, x, y);
    });
    if (ctx.FrameRefreshCallbacks)
        ctx.FrameRefreshCallbacks->push_back([graph, world = ctx.World, entity = ctx.Entity]
                                             { UpdateReadingMarker(*graph, *world, entity); });
    graph->SetSelectedKey(0);
    SyncKeyFields(*graph, *evField, *stopsField);
    return graph;
}

} // namespace GameEngine
