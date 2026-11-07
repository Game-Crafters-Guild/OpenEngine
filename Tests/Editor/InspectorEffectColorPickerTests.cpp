#include "InspectorRegistry.h"
#include "Inspectors/AtmosphericCloudLayerInspector.h"
#include "Inspectors/PostProcessEffects/BloomEffectInspector.h"
#include "Inspectors/PostProcessEffects/ColorFilterEffectInspector.h"
#include "Inspectors/InspectorUIHelpers.h"

#include "Components/Rendering/PostProcessEffects/AtmosphericCloudLayer.h"
#include "Components/Rendering/PostProcessEffects/BloomEffect.h"
#include "Components/Rendering/PostProcessEffects/ColorFilterEffect.h"
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

constexpr std::uint32_t kRed = 0xFFFF0000u;
constexpr std::uint32_t kGreen = 0xFF00FF00u;
constexpr std::uint32_t kBlue = 0xFF0000FFu;
constexpr float kOtherValue = 0.25f;

template<typename TComponent>
ECS::EntityHandle CreateComponent(ECS::World& world)
{
    TComponent component{};
    if constexpr (std::is_same_v<TComponent, Bloom>)
    {
        component.DepthVeilEnabled = true;
        component.LensDirtEnabled = true;
    }
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

Label* FindLabel(UIElement& element, const std::string& text)
{
    if (auto* label = dynamic_cast<Label*>(&element); label && label->GetText() == text)
        return label;
    for (const auto& child : element.GetChildren())
        if (Label* found = FindLabel(*child, text))
            return found;
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

    void OpenPicker()
    {
        Label* name = FindLabel(Root, GetParam().Row);
        ASSERT_NE(name, nullptr);
        UIElement* row = InspectorUI::RowOfControl(name);
        ASSERT_NE(row, nullptr);
        Label* value = FindLabel(*row, "(1.00, 1.00, 1.00)");
        ASSERT_NE(value, nullptr);
        UIEvent press{};
        press.Id = kEventMouseDown;
        press.Button = 0;
        value->DispatchEvent(press);
        ASSERT_TRUE(Callbacks);
        EXPECT_EQ(OpenedArgb, 0xFFFFFFFFu);
        EXPECT_FLOAT_EQ(OpenedIntensity, 1.0f);
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

INSTANTIATE_TEST_SUITE_P(EffectRows, InspectorEffectColorPickerTests, ::testing::Values(
    PickerCase{"BloomTint", "Tint", RegisterBloomEffectInspector, CreateComponent<Bloom>,
        ReadColor<Bloom, &Bloom::Tint>, SetOtherValue<Bloom, &Bloom::Intensity>, ReadOtherValue<Bloom, &Bloom::Intensity>, 1.0f},
    PickerCase{"BloomVeilTint", "Veil Tint", RegisterBloomEffectInspector, CreateComponent<Bloom>,
        ReadColor<Bloom, &Bloom::DepthVeilTint>, SetOtherValue<Bloom, &Bloom::Intensity>, ReadOtherValue<Bloom, &Bloom::Intensity>, 1.0f},
    PickerCase{"BloomVignette", "Vignette Color", RegisterBloomEffectInspector, CreateComponent<Bloom>,
        ReadColor<Bloom, &Bloom::LensDirtVignetteColor>, SetOtherValue<Bloom, &Bloom::Intensity>, ReadOtherValue<Bloom, &Bloom::Intensity>, 1.0f},
    PickerCase{"ColorFilter", "Color", RegisterColorFilterEffectInspector, CreateComponent<Filter>,
        ReadColor<Filter, &Filter::Color>, SetOtherValue<Filter, &Filter::Intensity>, ReadOtherValue<Filter, &Filter::Intensity>, 1.0f},
    PickerCase{"AtmosphericCloud", "CloudColor", RegisterAtmosphericCloudLayerInspector, CreateComponent<Cloud>,
        ReadColor<Cloud, &Cloud::CloudColor>, SetOtherValue<Cloud, &Cloud::SkyFill>, ReadOtherValue<Cloud, &Cloud::SkyFill>, 2.0f}),
    [](const ::testing::TestParamInfo<PickerCase>& parameter) { return parameter.param.Name; });
} // namespace
