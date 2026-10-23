#include <gtest/gtest.h>

#include "Components/Rendering/Light.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "EditorChangeNotifications.h"
#include "InspectorRegistry.h"
#include "Inspectors/LightInspector.h"
#include "UI/Controls/Label.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UndoRedo/UndoRedoService.h"

#include <chrono>
#include <string_view>
#include <thread>

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

class LightInspectorColorResetTests : public ::testing::TestWithParam<bool>
{
protected:
    ECS::World World;
    Editor::UndoRedoService Undo;
    Editor::EditorChangeNotifications Notifications;
    UIElement Root;
};

TEST_P(LightInspectorColorResetTests, DoubleClickRefreshesTheColorRowWithoutRebuilding)
{
    Components::Light light{};
    light.Type = Components::LightType::Ambient;
    light.Color[0] = 0.25f;
    light.Color[1] = 0.50f;
    light.Color[2] = 0.75f;
    const ECS::EntityHandle entity = World.Create(light).GetHandle();

    RegisterLightInspector();
    InspectorFn* inspector = InspectorRegistry::Get().TryGetComponentInspector<Components::Light>();
    ASSERT_NE(inspector, nullptr);
    InspectorContext context{};
    context.Parent = &Root;
    context.World = &World;
    context.Entity = entity;
    context.Undo = GetParam() ? &Undo : nullptr;
    context.ChangeNotifications = &Notifications;
    (*inspector)(context);

    Label* colorLabel = FindLabel(Root, "Color");
    Label* rgbLabel = FindLabel(Root, "(0.25, 0.50, 0.75)");
    ASSERT_NE(colorLabel, nullptr);
    ASSERT_NE(rgbLabel, nullptr);
    UIElement* field = rgbLabel->GetParent();
    ASSERT_NE(field, nullptr);
    ASSERT_EQ(field->GetChildren().size(), 2u);
    UIElement* swatch = field->GetChildren().front().get();
    EXPECT_EQ(swatch->Overrides().Get(Style::BackgroundColor), 0xFF4080BFu);

    UIEvent firstClick{};
    firstClick.Id = kEventMouseDown;
    firstClick.Button = 0;
    colorLabel->DispatchEvent(firstClick);
    EXPECT_EQ(rgbLabel->GetText(), "(0.25, 0.50, 0.75)");
    EXPECT_FALSE(Undo.CanUndo());

    // The handler measures whole milliseconds and rejects a zero-length interval.
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    UIEvent secondClick{};
    secondClick.Id = kEventMouseDown;
    secondClick.Button = 0;
    colorLabel->DispatchEvent(secondClick);

    const auto* reset = World.GetComponent<Components::Light>(entity);
    ASSERT_NE(reset, nullptr);
    EXPECT_FLOAT_EQ(reset->Color[0], 1.0f);
    EXPECT_FLOAT_EQ(reset->Color[1], 1.0f);
    EXPECT_FLOAT_EQ(reset->Color[2], 1.0f);
    EXPECT_EQ(rgbLabel->GetText(), "(1.00, 1.00, 1.00)");
    EXPECT_EQ(swatch->Overrides().Get(Style::BackgroundColor), 0xFFFFFFFFu);

    if (GetParam())
    {
        ASSERT_EQ(Undo.GetUndoCount(), 1u);
        Undo.Undo();
        const auto* restored = World.GetComponent<Components::Light>(entity);
        ASSERT_NE(restored, nullptr);
        EXPECT_FLOAT_EQ(restored->Color[0], light.Color[0]);
        EXPECT_FLOAT_EQ(restored->Color[1], light.Color[1]);
        EXPECT_FLOAT_EQ(restored->Color[2], light.Color[2]);
        Undo.Redo();
        EXPECT_FLOAT_EQ(World.GetComponent<Components::Light>(entity)->Color[0], 1.0f);
    }
    else
    {
        EXPECT_FALSE(Undo.CanUndo());
    }
}

INSTANTIATE_TEST_SUITE_P(WithAndWithoutUndo, LightInspectorColorResetTests, ::testing::Bool(),
                         [](const ::testing::TestParamInfo<bool>& info) {
                             return info.param ? "WithUndo" : "WithoutUndo";
                         });

} // namespace
