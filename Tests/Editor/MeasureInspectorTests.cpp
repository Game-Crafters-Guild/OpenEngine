#include "Components/Measure/MeasureComponent.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "EditorChangeNotifications.h"
#include "InspectorRegistry.h"
#include "Inspectors/MeasureInspector.h"
#include "UI/Controls/Label.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UndoRedo/UndoRedoService.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

using namespace GameEngine;

namespace
{
Label* FindLabel(UIElement& element, std::string_view text)
{
    if (auto* label = dynamic_cast<Label*>(&element); label && label->GetText() == text)
        return label;
    for (const auto& child : element.GetChildren())
        if (Label* label = FindLabel(*child, text))
            return label;
    return nullptr;
}

class MeasureInspectorColorPickerTests : public ::testing::TestWithParam<bool>
{
protected:
    void SetUp() override
    {
        Components::MeasureComponent measure{};
        measure.Color[0] = 1.0f;
        measure.Color[1] = 1.0f;
        measure.Color[2] = 1.0f;
        Entity = World.Create(measure).GetHandle();
        RegisterMeasureInspector();
        InspectorFn* inspector = InspectorRegistry::Get().TryGetComponentInspector<Components::MeasureComponent>();
        ASSERT_NE(inspector, nullptr);
        InspectorContext context{};
        context.Parent = &Root;
        context.World = &World;
        context.Entity = Entity;
        context.Undo = &Undo;
        context.ChangeNotifications = &Notifications;
        context.OpenColorPickerWindow = [this](std::uint32_t, float, ColorPickerCallbacks callbacks)
        {
            Picker = std::move(callbacks);
        };
        (*inspector)(context);
        Label* value = FindLabel(Root, "(1.00, 1.00, 1.00)");
        ASSERT_NE(value, nullptr);
        UIEvent press{};
        press.Id = kEventMouseDown;
        press.Button = 0;
        value->DispatchEvent(press);
        ASSERT_TRUE(Picker.has_value());
    }

    ECS::World World;
    ECS::EntityHandle Entity;
    Editor::UndoRedoService Undo;
    Editor::EditorChangeNotifications Notifications;
    UIElement Root;
    std::optional<ColorPickerCallbacks> Picker;
};

TEST_P(MeasureInspectorColorPickerTests, ResumesAfterSettlementAndInspectorRebuild)
{
    Picker->onValueChanging(0xFFFF0000u, 1.0f);
    ASSERT_TRUE(Undo.CommitPendingEdit());
    ASSERT_EQ(Undo.GetUndoCount(), 1u);
    Root.RemoveAllChildren();

    if (GetParam())
    {
        Picker->onApply(0xFF00FF00u, 1.0f);
        ASSERT_TRUE(Undo.CommitPendingEdit());
        EXPECT_FLOAT_EQ(World.GetComponent<Components::MeasureComponent>(Entity)->Color[1], 1.0f);
        ASSERT_EQ(Undo.GetUndoCount(), 2u);
        Undo.Undo();
        EXPECT_FLOAT_EQ(World.GetComponent<Components::MeasureComponent>(Entity)->Color[0], 1.0f);
        EXPECT_FLOAT_EQ(World.GetComponent<Components::MeasureComponent>(Entity)->Color[1], 0.0f);
        Undo.Redo();
        EXPECT_FLOAT_EQ(World.GetComponent<Components::MeasureComponent>(Entity)->Color[1], 1.0f);
    }
    else
    {
        Picker->onValueChanging(0xFF00FF00u, 1.0f);
        EXPECT_FLOAT_EQ(World.GetComponent<Components::MeasureComponent>(Entity)->Color[1], 1.0f);
        Picker->onCancel();
        EXPECT_FLOAT_EQ(World.GetComponent<Components::MeasureComponent>(Entity)->Color[0], 1.0f);
        EXPECT_FLOAT_EQ(World.GetComponent<Components::MeasureComponent>(Entity)->Color[1], 0.0f);
        EXPECT_FALSE(Undo.HasPendingEdit());
        ASSERT_EQ(Undo.GetUndoCount(), 1u);
    }
}

INSTANTIATE_TEST_SUITE_P(AfterSave, MeasureInspectorColorPickerTests, ::testing::Bool(),
    [](const ::testing::TestParamInfo<bool>& info) { return info.param ? "Apply" : "Cancel"; });
} // namespace
