#pragma once

#include "InspectorRegistry.h"
#include "Inspectors/InspectorColorSwatchRow.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Types/ColorUtils.h"
#include "UI/Controls/Label.h"
#include "UndoRedo/UndoRedoService.h"

#include <memory>
#include <string>

namespace GameEngine::InspectorUI
{

/// A component's color member edited through the color picker: a chip and its value, a click opens the
/// picker, the edit is one undo step, cancelling puts the value back. The member is a color as a picker
/// shows it, linear, each channel 0 to 1 (no intensity); the chip and the picker speak sRGB bytes, so the value is
/// encoded on the way out and decoded on the way back.
template<typename TComponent>
struct ComponentColorBinding
{
    using Member = float (TComponent::*)[3];

    ECS::World* World = nullptr;
    ECS::EntityHandle Entity{};
    Editor::EditorChangeNotifications* Notifications = nullptr;
    Editor::UndoRedoService* Undo = nullptr;
    OpenColorPickerWindowFn OpenPicker;
    Member Color = nullptr;
    std::string ChangeLabel;
    ColorSwatchRow Row;
};

template<typename TComponent>
std::uint32_t ComponentColorArgb(const ComponentColorBinding<TComponent>& binding)
{
    const TComponent* component = binding.World->template GetComponent<TComponent>(binding.Entity);
    if (!component)
        return 0xFF000000u;
    const float* rgb = component->*binding.Color;
    const ColorSRGB srgb = ColorLinear::FromLinear01(rgb[0], rgb[1], rgb[2]).ToSRGB();
    return ColorUtils::PackArgbRounded(srgb.r, srgb.g, srgb.b, 1.0f);
}

template<typename TComponent>
void RefreshComponentColorRow(const ComponentColorBinding<TComponent>& binding)
{
    const TComponent* component = binding.World->template GetComponent<TComponent>(binding.Entity);
    if (!component || !binding.Row.Swatch || !binding.Row.Value)
        return;
    const float* rgb = component->*binding.Color;
    StyleColorSwatch(binding.Row.Swatch, ComponentColorArgb(binding));
    binding.Row.Value->SetText(FormatColorRgb(rgb[0], rgb[1], rgb[2]));
}

template<typename TComponent>
void WriteComponentColor(TComponent& component, typename ComponentColorBinding<TComponent>::Member member,
                         std::uint32_t argb)
{
    float linear[4] = {};
    ColorUtils::ARGBToLinearFloats(argb, linear);
    (component.*member)[0] = linear[0];
    (component.*member)[1] = linear[1];
    (component.*member)[2] = linear[2];
}

// One picker session: every change previews into the interactive edit, apply commits it as one undo
// step, cancel restores the value from before.
template<typename TComponent>
void OpenComponentColorPicker(const std::shared_ptr<ComponentColorBinding<TComponent>>& binding)
{
    if (!binding->OpenPicker || !binding->World->template GetComponent<TComponent>(binding->Entity))
        return;
    using Edit = Editor::UndoRedoService::InteractiveEdit;
    auto edit = std::make_shared<Edit>();
    if (binding->Undo)
        *edit = binding->Undo->BeginInteractiveEdit(
            binding->ChangeLabel, InspectorDrag::MakeComponentSnapshotTarget<TComponent>(binding->World, binding->Entity,
                                                                          binding->Notifications, binding->ChangeLabel));
    auto preview = [binding, edit](std::uint32_t argb, float)
    {
        if (*edit)
            edit->Preview([&] {
                if (TComponent* c = binding->World->template GetComponentForWrite<TComponent>(binding->Entity))
                    WriteComponentColor(*c, binding->Color, argb);
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
    binding->OpenPicker(ComponentColorArgb(*binding), 1.0f, std::move(callbacks));
}

/// Adds the row and wires it: a click on the chip or the value opens the picker.
template<typename TComponent>
void AddComponentColorRow(UIElement* parent, const std::string& label, const char* tooltip,
                          ComponentColorBinding<TComponent> binding)
{
    auto shared = std::make_shared<ComponentColorBinding<TComponent>>(std::move(binding));
    const TComponent* component = shared->World->template GetComponent<TComponent>(shared->Entity);
    if (!component)
        return;
    const float* rgb = component->*shared->Color;
    shared->Row = AddColorSwatchRow(parent, label, tooltip, ComponentColorArgb(*shared),
                                    FormatColorRgb(rgb[0], rgb[1], rgb[2]));
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
}

} // namespace GameEngine::InspectorUI
