#pragma once

#include "InspectorRegistry.h"
#include "Inspectors/InspectorColorSwatchRow.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Platform/SystemMetrics.h"
#include "UI/Controls/Label.h"
#include "UndoRedo/UndoRedoService.h"

#include <chrono>
#include <memory>
#include <string>

namespace GameEngine::InspectorUI
{

/// A component's linear color member on a chip and the picker. An edit, a cancel or a label double-click
/// reset is one undo step; the row edits red, green and blue, never alpha; the chip and the picker share one
/// sRGB encoding (HdrColorToPickerState).
template<typename TComponent>
struct ComponentColorBinding
{
    using Member = ColorLinear TComponent::*;

    ECS::World* World = nullptr;
    ECS::EntityHandle Entity{};
    Editor::EditorChangeNotifications* Notifications = nullptr;
    Editor::UndoRedoService* Undo = nullptr;
    OpenColorPickerWindowFn OpenPicker;
    Member Color = nullptr;
    std::string ChangeLabel;
    /// The brightest a channel may be: 1 keeps the color within 0 to 1 and the picker's intensity is not
    /// used; above 1 the picker carries the brightness above 1 as its intensity, up to this value.
    float MaxIntensity = 1.0f;
    ColorSwatchRow Row;
    /// When the label was last pressed; a second press within the double-click interval resets the color.
    std::chrono::steady_clock::time_point LastLabelClick{};
};

// What the chip shows and the picker opens on: the member's red, green and blue, opaque, as the row never edits
// the alpha.
template<typename TComponent>
ColorPickerState ComponentColorPickerState(const TComponent& component, const ComponentColorBinding<TComponent>& binding)
{
    const ColorLinear& color = component.*binding.Color;
    return HdrColorToPickerState(ColorLinear(color.r, color.g, color.b), binding.MaxIntensity);
}

template<typename TComponent>
void RefreshComponentColorRow(const ComponentColorBinding<TComponent>& binding)
{
    const TComponent* component = binding.World->template GetComponent<TComponent>(binding.Entity);
    if (!component || !binding.Row.Swatch || !binding.Row.Value)
        return;
    const ColorLinear& color = component->*binding.Color;
    StyleColorSwatch(binding.Row.Swatch, ComponentColorPickerState(*component, binding).Argb);
    binding.Row.Value->SetText(FormatColorRgb(color.r, color.g, color.b));
}

template<typename TComponent>
void WriteComponentColor(TComponent& component, const ComponentColorBinding<TComponent>& binding, std::uint32_t argb,
                         float intensity)
{
    const ColorLinear picked = PickerStateToHdrColor(argb, intensity, binding.MaxIntensity);
    ColorLinear& color = component.*binding.Color;
    color.r = picked.r;
    color.g = picked.g;
    color.b = picked.b;
}

// One picker session: every change previews into the interactive edit, apply commits it as one undo
// step, cancel restores the value from before.
template<typename TComponent>
void OpenComponentColorPicker(const std::shared_ptr<ComponentColorBinding<TComponent>>& binding)
{
    const TComponent* component = binding->World->template GetComponent<TComponent>(binding->Entity);
    if (!binding->OpenPicker || !component)
        return;
    const ColorPickerState state = ComponentColorPickerState(*component, *binding);
    using Edit = Editor::UndoRedoService::InteractiveEdit;
    auto edit = std::make_shared<Edit>();
    if (binding->Undo)
        *edit = binding->Undo->BeginInteractiveEdit(
            binding->ChangeLabel, InspectorDrag::MakeComponentSnapshotTarget<TComponent>(binding->World, binding->Entity,
                                                                          binding->Notifications, binding->ChangeLabel));
    auto preview = [binding, edit](std::uint32_t argb, float intensity)
    {
        if (*edit)
            edit->Preview([&] {
                if (TComponent* c = binding->World->template GetComponentForWrite<TComponent>(binding->Entity))
                    WriteComponentColor(*c, *binding, argb, intensity);
            });
        RefreshComponentColorRow(*binding);
    };
    ColorPickerCallbacks callbacks;
    callbacks.onValueChanging = preview;
    callbacks.onApply = [preview, edit](std::uint32_t argb, float intensity)
    {
        preview(argb, intensity);
        if (*edit)
            edit->Commit();
    };
    callbacks.onCancel = [binding, edit]()
    {
        if (*edit)
            edit->Cancel();
        RefreshComponentColorRow(*binding);
    };
    binding->OpenPicker(state.Argb, state.Intensity, std::move(callbacks));
}

// A second press on the label within the platform's double-click interval resets the color's red, green and
// blue to the member's default as one undo step.
template<typename TComponent>
void HandleComponentColorLabelPress(ComponentColorBinding<TComponent>& binding, UIEvent& event)
{
    if (event.Button != 0)
        return;
    const auto now = std::chrono::steady_clock::now();
    const bool isDoubleClick = binding.LastLabelClick.time_since_epoch().count() != 0 &&
                               (now - binding.LastLabelClick) < Platform::GetDoubleClickInterval();
    binding.LastLabelClick = isDoubleClick ? std::chrono::steady_clock::time_point{} : now;
    if (!isDoubleClick)
        return;
    event.Stop();
    const ColorLinear defaultColor = TComponent{}.*binding.Color;
    InspectorDrag::CommitComponentWithUndo<TComponent>(
        binding.World, binding.Entity, binding.Notifications, binding.Undo, binding.ChangeLabel,
        [&binding, defaultColor](TComponent& component)
        {
            ColorLinear& color = component.*binding.Color;
            color.r = defaultColor.r;
            color.g = defaultColor.g;
            color.b = defaultColor.b;
        });
    RefreshComponentColorRow(binding);
}

/// Adds the row and wires it: a click on the chip or the value opens the picker, a double-click on the label
/// resets the color.
template<typename TComponent>
void AddComponentColorRow(UIElement* parent, const std::string& label, const char* tooltip,
                          ComponentColorBinding<TComponent> binding)
{
    auto shared = std::make_shared<ComponentColorBinding<TComponent>>(std::move(binding));
    const TComponent* component = shared->World->template GetComponent<TComponent>(shared->Entity);
    if (!component)
        return;
    const ColorLinear& color = component->*shared->Color;
    shared->Row = AddColorSwatchRow(parent, label, tooltip, ComponentColorPickerState(*component, *shared).Argb,
                                    FormatColorRgb(color.r, color.g, color.b));
    auto open = [shared](UIEvent& event)
    {
        if (event.Button != 0)
            return;
        event.Stop();
        OpenComponentColorPicker(shared);
    };
    if (shared->Row.Swatch)
        shared->Row.Swatch->RegisterEventHandler(kEventMouseDown, open);
    if (shared->Row.Value)
        shared->Row.Value->RegisterEventHandler(kEventMouseDown, open);
    if (shared->Row.Name)
        shared->Row.Name->RegisterEventHandler(kEventMouseDown,
                                               [shared](UIEvent& event) { HandleComponentColorLabelPress(*shared, event); });
}

} // namespace GameEngine::InspectorUI
