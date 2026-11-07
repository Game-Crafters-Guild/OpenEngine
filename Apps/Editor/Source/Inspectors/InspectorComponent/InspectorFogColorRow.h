#pragma once

#include "InspectorRegistry.h"
#include "Inspectors/InspectorColorSwatchRow.h"
#include "Inspectors/InspectorComponent/InspectorColorPickerEdit.h"
#include "Inspectors/InspectorComponent/InspectorComponentRowHelpers.h"
#include "Platform/SystemMetrics.h"
#include "Types/ColorUtils.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>

namespace GameEngine::InspectorUI
{
/// The HDR color row shared by height fog and volumetric fog.
template<typename TComponent, auto TColor>
requires std::is_member_object_pointer_v<decltype(TColor)>
class InspectorFogColorRow
{
public:
    /// Adds the color chip, picker, value text, and label reset for one component member.
    static void Add(UIElement* parent, const char* label, const char* tooltip, const InspectorContext& context,
                    ColorLinear defaultValue, const char* changeLabel)
    {
        if (!parent || !context.World || !context.Entity.IsValid())
            return;
        const auto* component = context.World->GetComponent<TComponent>(context.Entity);
        if (!component)
            return;
        const auto& color = component->*TColor;
        const ColorSwatchRow row = AddColorSwatchRow(parent, label, tooltip, ReadState(*component).Argb,
                                                    FormatColorRgb(color[0], color[1], color[2]));

        auto refresh = [context, swatchRef = UIElement::MakeWeakRef(row.Swatch),
                        valueRef = UIElement::MakeWeakRef(row.Value)]
        {
            Refresh(context, swatchRef, valueRef);
        };
        auto press = InspectorColorPickerEdit::MakeComponentPressHandler<TComponent>(
            context, changeLabel, changeLabel, ReadState, WriteColor, refresh);
        row.Swatch->RegisterEventHandler(kEventMouseDown, press);
        row.Value->RegisterEventHandler(kEventMouseDown, press);
        if (row.Name)
        {
            auto lastClick = std::make_shared<std::chrono::steady_clock::time_point>();
            row.Name->RegisterEventHandler(kEventMouseDown,
                [context, defaultValue, changeLabel, refresh, lastClick](UIEvent& event)
                {
                    ResetOnDoubleClick(context, event, defaultValue, changeLabel, refresh, *lastClick);
                });
        }
    }

private:
    static constexpr float kMaxIntensity = 8.0f;
    static constexpr float kBlackThreshold = 1e-6f;

    static ColorPickerState ReadState(const TComponent& component)
    {
        const auto& color = component.*TColor;
        const float maximum = std::max({color[0], color[1], color[2]});
        if (maximum <= kBlackThreshold)
            return {0xFF000000u, 1.0f};
        const float intensity = std::clamp(maximum, 1.0f, kMaxIntensity);
        const float inverse = 1.0f / intensity;
        return {ColorUtils::PackArgbRounded(color[0] * inverse, color[1] * inverse, color[2] * inverse, 1.0f), intensity};
    }

    static void WriteColor(TComponent& component, std::uint32_t argb, float intensity)
    {
        const float resolved = std::clamp(intensity, 1.0f, kMaxIntensity);
        const ColorLinear picked = ColorUtils::UnpackArgb(argb);
        auto& color = component.*TColor;
        color[0] = picked.r * resolved;
        color[1] = picked.g * resolved;
        color[2] = picked.b * resolved;
    }

    static void Refresh(const InspectorContext& context, UIElement::WeakRef<UIElement> swatchRef,
                        UIElement::WeakRef<Label> valueRef)
    {
        const auto* component = context.World->GetComponent<TComponent>(context.Entity);
        UIElement* swatch = swatchRef.Get();
        Label* value = valueRef.Get();
        if (!component || !swatch || !value)
            return;
        StyleColorSwatch(swatch, ReadState(*component).Argb);
        const auto& color = component->*TColor;
        value->SetText(FormatColorRgb(color[0], color[1], color[2]));
    }

    static void WriteDefault(TComponent& component, const ColorLinear& value)
    {
        auto& color = component.*TColor;
        color[0] = value.r;
        color[1] = value.g;
        color[2] = value.b;
    }

    static void ResetOnDoubleClick(const InspectorContext& context, UIEvent& event, const ColorLinear& defaultValue,
                                   const char* changeLabel, const std::function<void()>& refresh,
                                   std::chrono::steady_clock::time_point& lastClick)
    {
        if (event.Button != 0)
            return;
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastClick);
        if (elapsed <= std::chrono::milliseconds::zero() || elapsed >= Platform::GetDoubleClickInterval())
        {
            lastClick = now;
            return;
        }
        event.Stop();
        InspectorDrag::CommitComponentWithUndo<TComponent>(context.World, context.Entity, context.ChangeNotifications,
            context.Undo, changeLabel, [defaultValue](TComponent& component) { WriteDefault(component, defaultValue); });
        lastClick = {};
        refresh();
    }
};
} // namespace GameEngine::InspectorUI
