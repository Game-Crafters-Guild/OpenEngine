#include "InspectorRegistry.h"
#include "Inspectors/AtmosphericCloudLayerInspector.h"
#include "Inspectors/PostProcessEffects/BloomEffectInspector.h"
#include "Inspectors/PostProcessEffects/ColorFilterEffectInspector.h"
#include "Inspectors/PostProcessEffects/FilmSimulationEffectInspector.h"
#include "Inspectors/PostProcessEffects/HeightFogEffectInspector.h"
#include "Inspectors/PostProcessEffects/VolumetricFogEffectInspector.h"
#include "Inspectors/PostProcessEffects/VhsEffectInspector.h"
#include "Inspectors/PostProcessEffects/VignetteEffectInspector.h"
#include "Inspectors/InspectorUIHelpers.h"

#include "Components/Rendering/PostProcessEffects/AtmosphericCloudLayer.h"
#include "Components/Rendering/PostProcessEffects/BloomEffect.h"
#include "Components/Rendering/PostProcessEffects/ColorFilterEffect.h"
#include "Components/Rendering/PostProcessEffects/FilmSimulationEffect.h"
#include "Components/Rendering/PostProcessEffects/HeightFogEffect.h"
#include "Components/Rendering/PostProcessEffects/VolumetricFogEffect.h"
#include "Components/Rendering/PostProcessEffects/VhsEffect.h"
#include "Components/Rendering/PostProcessEffects/VignetteEffect.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Scripting/ScriptsConfig.h"
#include "Types/Color.h"
#include "UI/Controls/Label.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UndoRedo/UndoRedoService.h"

#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

using namespace GameEngine;

namespace
{
using Bloom = Components::BloomEffect;
using Filter = Components::ColorFilterEffect;
using Cloud = Components::AtmosphericCloudLayer;
using Film = Components::FilmSimulationEffect;
using HeightFog = Components::HeightFogEffect;
using VolumeFog = Components::VolumetricFogEffect;
using Vhs = Components::VhsEffect;
using Vignette = Components::VignetteEffect;

constexpr std::uint32_t kRed = 0xFFFF0000u;
constexpr std::uint32_t kGreen = 0xFF00FF00u;
constexpr std::uint32_t kBlue = 0xFF0000FFu;
constexpr std::uint32_t kMixedColor = 0xFF33FF99u;
constexpr float kOtherValue = 0.25f;

template<typename TComponent, auto TColor>
ECS::EntityHandle CreateComponent(ECS::World& world)
{
    TComponent component{};
    auto& color = component.*TColor;
    color[0] = color[1] = color[2] = 1.0f;
    if constexpr (std::is_same_v<TComponent, Bloom>)
    {
        component.DepthVeilEnabled = true;
        component.LensDirtEnabled = true;
    }
    if constexpr (std::is_same_v<TComponent, HeightFog>)
    {
        component.ColorMode = Components::HeightFogColorMode::FixedColor;
        component.FogGlowEnabled = true;
    }
    if constexpr (std::is_same_v<TComponent, VolumeFog>)
        component.FogGlowEnabled = true;
    return world.Create(component).GetHandle();
}

template<typename TComponent, auto TColor>
ColorLinear ReadColor(const ECS::World& world, ECS::EntityHandle entity)
{
    const auto& color = world.GetComponent<TComponent>(entity)->*TColor;
    return {color[0], color[1], color[2]};
}

template<typename TComponent, auto TMember>
void SetOtherValue(ECS::World& world, ECS::EntityHandle entity)
{
    world.GetComponentForWrite<TComponent>(entity)->*TMember = kOtherValue;
}

template<typename TComponent, auto TMember>
float ReadOtherValue(const ECS::World& world, ECS::EntityHandle entity)
{
    return world.GetComponent<TComponent>(entity)->*TMember;
}

struct PickerCase
{
    const char* Name;
    const char* Row;
    void (*Register)();
    ECS::EntityHandle (*Create)(ECS::World&);
    ColorLinear (*Read)(const ECS::World&, ECS::EntityHandle);
    void (*SetOther)(ECS::World&, ECS::EntityHandle);
    float (*ReadOther)(const ECS::World&, ECS::EntityHandle);
    float Intensity;
};

template<typename TComponent, auto TColor, auto TOther>
PickerCase MakePickerCase(const char* name, const char* row, void (*registerInspector)(), float intensity)
{
    return {name, row, registerInspector, CreateComponent<TComponent, TColor>, ReadColor<TComponent, TColor>,
            SetOtherValue<TComponent, TOther>, ReadOtherValue<TComponent, TOther>, intensity};
}

Label* FindLabel(UIElement& element, const std::string& text)
{
    if (auto* label = dynamic_cast<Label*>(&element); label && label->GetText() == text)
        return label;
    for (const auto& child : element.GetChildren())
        if (Label* found = FindLabel(*child, text))
            return found;
    return nullptr;
}

Label* FindColorValue(UIElement& row)
{
    for (const auto& field : row.GetChildren())
        if (field->HasClass("inspector-field"))
            for (const auto& control : field->GetChildren())
                if (auto* label = dynamic_cast<Label*>(control.get()); label && label->HasClass("inspector-text"))
                    return label;
    return nullptr;
}

class InspectorEffectColorPickerTests : public ::testing::TestWithParam<PickerCase>
{
protected:
    static void SetUpTestSuite()
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (engine.IsInitialized())
            return;
        ScriptsConfig scripts{};
        scripts.disableClr = true;
        scripts.enableHotReload = false;
        scripts.enableAsyncHotReload = false;
        scripts.enableAutoProjectGeneration = false;
        engine.SetScriptsConfig(scripts);
        ApplicationConfig config{};
        config.AssetDirectory = ".";
        config.WorkspaceDirectory = ".";
        config.EnableEditor = true;
        ASSERT_TRUE(engine.Initialize(config));
    }

    void SetUp() override
    {
        GetParam().Register();
        Entity = GetParam().Create(World);
        BuildInspector(true);
    }

    void BuildInspector(bool withUndo)
    {
        Root.RemoveAllChildren();
        InspectorContext context{};
        context.Parent = &Root;
        context.World = &World;
        context.Entity = Entity;
        context.Undo = withUndo ? &Undo : nullptr;
        context.OpenColorPickerWindow = [this](std::uint32_t argb, float intensity, ColorPickerCallbacks callbacks)
        {
            OpenedArgb = argb;
            OpenedIntensity = intensity;
            Callbacks = std::move(callbacks);
        };
        const auto typeId = World.GetEntityArchetype(Entity)->GetSignature().GetComponents()[0];
        InspectorFn* inspector = InspectorRegistry::Get().TryGetComponentInspector(typeId);
        ASSERT_NE(inspector, nullptr);
        (*inspector)(context);
    }

    void OpenPicker(std::uint32_t expectedArgb = 0xFFFFFFFFu, float expectedIntensity = 1.0f)
    {
        Label* name = FindLabel(Root, GetParam().Row);
        ASSERT_NE(name, nullptr);
        UIElement* row = InspectorUI::RowOfControl(name);
        ASSERT_NE(row, nullptr);
        Label* value = FindColorValue(*row);
        ASSERT_NE(value, nullptr);
        UIEvent press{};
        press.Id = kEventMouseDown;
        press.Button = 0;
        value->DispatchEvent(press);
        ASSERT_TRUE(Callbacks);
        EXPECT_EQ(OpenedArgb, expectedArgb);
        EXPECT_FLOAT_EQ(OpenedIntensity, expectedIntensity);
    }

    void ExpectColor(float red, float green, float blue)
    {
        const ColorLinear actual = GetParam().Read(World, Entity);
        EXPECT_FLOAT_EQ(actual.r, red);
        EXPECT_FLOAT_EQ(actual.g, green);
        EXPECT_FLOAT_EQ(actual.b, blue);
    }

    ECS::World World;
    ECS::EntityHandle Entity{};
    Editor::UndoRedoService Undo;
    UIElement Root;
    std::optional<ColorPickerCallbacks> Callbacks;
    std::uint32_t OpenedArgb = 0;
    float OpenedIntensity = 0.0f;
};

TEST_P(InspectorEffectColorPickerTests, PreviewAndApplyResumeAfterRepeatedSettlementAndInspectorRebuild)
{
    OpenPicker();
    ASSERT_TRUE(Callbacks);
    const float intensity = GetParam().Intensity;
    Callbacks->onValueChanging(kRed, intensity);
    ASSERT_TRUE(Undo.CommitPendingEdit());
    Undo.MarkSaved(&World);
    BuildInspector(true);
    Callbacks->onValueChanging(kGreen, intensity);
    EXPECT_FALSE(Undo.IsAtSavedRevision(&World));
    ASSERT_TRUE(Undo.CommitPendingEdit());
    Callbacks->onApply(kBlue, intensity);
    ASSERT_EQ(Undo.GetUndoCount(), 3u);
    ExpectColor(0.0f, 0.0f, intensity);
    Undo.Undo();
    ExpectColor(0.0f, intensity, 0.0f);
    Undo.Undo();
    ExpectColor(intensity, 0.0f, 0.0f);
    EXPECT_TRUE(Undo.IsAtSavedRevision(&World));
    Undo.Redo();
    Undo.Redo();
    ExpectColor(0.0f, 0.0f, intensity);
}

TEST_P(InspectorEffectColorPickerTests, CancelRestoresTheSavedColorAndKeepsLaterComponentFields)
{
    OpenPicker();
    ASSERT_TRUE(Callbacks);
    const float intensity = GetParam().Intensity;
    Callbacks->onValueChanging(kRed, intensity);
    ASSERT_TRUE(Undo.CommitPendingEdit());
    Undo.MarkSaved(&World);
    GetParam().SetOther(World, Entity);
    Callbacks->onValueChanging(kGreen, intensity);
    Callbacks->onCancel();
    ExpectColor(intensity, 0.0f, 0.0f);
    EXPECT_FLOAT_EQ(GetParam().ReadOther(World, Entity), kOtherValue);
    EXPECT_EQ(Undo.GetUndoCount(), 1u);
    EXPECT_TRUE(Undo.IsAtSavedRevision(&World));
}

TEST_P(InspectorEffectColorPickerTests, ApplyResumesAfterSavingAnUnchangedPicker)
{
    OpenPicker();
    ASSERT_TRUE(Callbacks);
    ASSERT_TRUE(Undo.CommitPendingEdit());
    Undo.MarkSaved(&World);
    Callbacks->onApply(kGreen, GetParam().Intensity);
    ASSERT_EQ(Undo.GetUndoCount(), 1u);
    Undo.Undo();
    ExpectColor(1.0f, 1.0f, 1.0f);
}

TEST_P(InspectorEffectColorPickerTests, PreviewAndApplyWorkWithoutAnUndoService)
{
    BuildInspector(false);
    OpenPicker();
    ASSERT_TRUE(Callbacks);
    Callbacks->onValueChanging(kRed, GetParam().Intensity);
    ExpectColor(GetParam().Intensity, 0.0f, 0.0f);
    Callbacks->onApply(kBlue, GetParam().Intensity);
    ExpectColor(0.0f, 0.0f, GetParam().Intensity);
}

TEST_P(InspectorEffectColorPickerTests, CancelingAnUnchangedReopenedPickerPreservesThePreviousApply)
{
    OpenPicker();
    ASSERT_TRUE(Callbacks);
    const float intensity = GetParam().Intensity;
    Callbacks->onApply(kMixedColor, intensity);
    BuildInspector(true);
    OpenPicker(kMixedColor, intensity);
    Callbacks->onCancel();
    ExpectColor(0.2f * intensity, intensity, 0.6f * intensity);
    EXPECT_EQ(Undo.GetUndoCount(), 1u);
    Undo.Undo();
    ExpectColor(1.0f, 1.0f, 1.0f);
    Undo.Redo();
    ExpectColor(0.2f * intensity, intensity, 0.6f * intensity);
}

TEST_P(InspectorEffectColorPickerTests, CancelingAReopenedPreviewRestoresTheAppliedColor)
{
    OpenPicker();
    ASSERT_TRUE(Callbacks);
    const float intensity = GetParam().Intensity;
    Callbacks->onApply(kRed, intensity);
    BuildInspector(true);
    OpenPicker(kRed, intensity);
    Callbacks->onValueChanging(kBlue, intensity);
    ExpectColor(0.0f, 0.0f, intensity);
    Callbacks->onCancel();
    ExpectColor(intensity, 0.0f, 0.0f);
    EXPECT_EQ(Undo.GetUndoCount(), 1u);
    Undo.Undo();
    ExpectColor(1.0f, 1.0f, 1.0f);
    Undo.Redo();
    ExpectColor(intensity, 0.0f, 0.0f);
}

INSTANTIATE_TEST_SUITE_P(EffectRows, InspectorEffectColorPickerTests, ::testing::Values(
    MakePickerCase<Bloom, &Bloom::Tint, &Bloom::Intensity>("BloomTint", "Tint", RegisterBloomEffectInspector, 1.0f),
    MakePickerCase<Bloom, &Bloom::DepthVeilTint, &Bloom::Intensity>("BloomVeilTint", "Veil Tint", RegisterBloomEffectInspector, 1.0f),
    MakePickerCase<Bloom, &Bloom::LensDirtVignetteColor, &Bloom::Intensity>("BloomVignette", "Vignette Color", RegisterBloomEffectInspector, 1.0f),
    MakePickerCase<Filter, &Filter::Color, &Filter::Intensity>("ColorFilter", "Color", RegisterColorFilterEffectInspector, 1.0f),
    MakePickerCase<Cloud, &Cloud::CloudColor, &Cloud::SkyFill>("AtmosphericCloud", "CloudColor", RegisterAtmosphericCloudLayerInspector, 2.0f),
    MakePickerCase<Film, &Film::HalationTint, &Film::HalationIntensity>("FilmHalation", "Halation Tint", RegisterFilmSimulationEffectInspector, 2.0f),
    MakePickerCase<HeightFog, &HeightFog::Emissive, &HeightFog::Density>("HeightFogEmissive", "Emissive", RegisterHeightFogEffectInspector, 2.0f),
    MakePickerCase<HeightFog, &HeightFog::GradientLowColor, &HeightFog::Density>("HeightFogGradientLow", "Gradient Low", RegisterHeightFogEffectInspector, 2.0f),
    MakePickerCase<HeightFog, &HeightFog::GradientHighColor, &HeightFog::Density>("HeightFogGradientHigh", "Gradient High", RegisterHeightFogEffectInspector, 2.0f),
    MakePickerCase<HeightFog, &HeightFog::SunColor, &HeightFog::Density>("HeightFogSun", "Sun Color", RegisterHeightFogEffectInspector, 2.0f),
    MakePickerCase<HeightFog, &HeightFog::FogGlowTint, &HeightFog::Density>("HeightFogGlow", "Tint", RegisterHeightFogEffectInspector, 2.0f),
    MakePickerCase<VolumeFog, &VolumeFog::Albedo, &VolumeFog::Density>("VolumeFogAlbedo", "Albedo", RegisterVolumetricFogEffectInspector, 2.0f),
    MakePickerCase<VolumeFog, &VolumeFog::Emission, &VolumeFog::Density>("VolumeFogEmission", "Emission", RegisterVolumetricFogEffectInspector, 2.0f),
    MakePickerCase<VolumeFog, &VolumeFog::SunScatteringTint, &VolumeFog::Density>("VolumeFogSun", "Sun Tint", RegisterVolumetricFogEffectInspector, 2.0f),
    MakePickerCase<VolumeFog, &VolumeFog::AmbientScatteringTint, &VolumeFog::Density>("VolumeFogAmbient", "Ambient Tint", RegisterVolumetricFogEffectInspector, 2.0f),
    MakePickerCase<VolumeFog, &VolumeFog::GradientLowTint, &VolumeFog::Density>("VolumeFogGradientLow", "Gradient Low", RegisterVolumetricFogEffectInspector, 2.0f),
    MakePickerCase<VolumeFog, &VolumeFog::GradientHighTint, &VolumeFog::Density>("VolumeFogGradientHigh", "Gradient High", RegisterVolumetricFogEffectInspector, 2.0f),
    MakePickerCase<VolumeFog, &VolumeFog::FogGlowTint, &VolumeFog::Density>("VolumeFogGlow", "Tint", RegisterVolumetricFogEffectInspector, 2.0f),
    MakePickerCase<Vhs, &Vhs::DateBurnColor, &Vhs::Intensity>("VhsDateBurn", "Date Burn Color", RegisterVhsEffectInspector, 1.0f),
    MakePickerCase<Vhs, &Vhs::OverlayColor, &Vhs::Intensity>("VhsTransport", "Transport Color", RegisterVhsEffectInspector, 1.0f),
    MakePickerCase<Vignette, &Vignette::Color, &Vignette::Intensity>("Vignette", "Color", RegisterVignetteEffectInspector, 1.0f)),
    [](const ::testing::TestParamInfo<PickerCase>& parameter) { return parameter.param.Name; });
} // namespace
