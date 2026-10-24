// The default inspector's row and option words: a field or an enumerator marked "// @ge-label"
// shows its label, and an unmarked one shows its identifier as words. Built in process over a
// live World, as in RenderLayersRowTests.

#include <gtest/gtest.h>

#include "InspectorRegistry.h"
#include "Inspectors/DefaultComponentInspector.h"

#include "Components/Rendering/DDGIVolume.h"
#include "Components/Rendering/ReflectionProbe.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/Label.h"
#include "UI/UIElement.h"
#include "UndoRedo/UndoRedoService.h"

#include <string>

using namespace GameEngine;

namespace
{

// The inspector row whose label reads `labelText`, or nullptr.
UIElement* FindRow(UIElement& element, const std::string& labelText)
{
    for (const auto& child : element.GetChildren())
    {
        if (child->HasClass("inspector-row"))
            for (const auto& cell : child->GetChildren())
                for (const auto& part : cell->GetChildren())
                    if (auto* label = dynamic_cast<Label*>(part.get()); label && label->GetText() == labelText)
                        return child.get();
        if (UIElement* found = FindRow(*child, labelText))
            return found;
    }
    return nullptr;
}

Dropdown* FindDropdown(UIElement& element)
{
    if (auto* dropdown = dynamic_cast<Dropdown*>(&element))
        return dropdown;
    for (const auto& child : element.GetChildren())
        if (Dropdown* found = FindDropdown(*child))
            return found;
    return nullptr;
}

class InspectorLabelRowTest : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (!engine.IsInitialized())
        {
            ApplicationConfig config{};
            config.AssetDirectory = ".";
            config.WorkspaceDirectory = ".";
            config.EnableEditor = true;
            ASSERT_TRUE(engine.Initialize(config));
        }
    }

    template <class T>
    void Render(const T& component)
    {
        const ECS::EntityHandle entity = world.Create(component).GetHandle();
        InspectorContext context;
        context.Parent = &parent;
        context.World = &world;
        context.Entity = entity;
        context.Undo = &undo;
        ASSERT_TRUE(RenderDefaultComponentInspector(context, ECS::GetComponentTypeId<T>()));
    }

    ECS::World world{nullptr};
    Editor::UndoRedoService undo;
    UIElement parent{"div"};
};

} // namespace

TEST_F(InspectorLabelRowTest, ALabeledFieldShowsItsLabelAndAnUnlabeledOneItsWords)
{
    ASSERT_NO_FATAL_FAILURE(Render(Components::DDGIVolume{}));
    EXPECT_NE(FindRow(parent, "Rays / Probe"), nullptr);
    EXPECT_EQ(FindRow(parent, "Rays Per Probe"), nullptr);
    EXPECT_NE(FindRow(parent, "Bounce Intensity"), nullptr);
}

TEST_F(InspectorLabelRowTest, ALabeledEnumeratorShowsItsLabel)
{
    Components::ReflectionProbe probe{};
    probe.UpdateMode = Components::ReflectionProbeUpdateMode::Realtime;
    ASSERT_NO_FATAL_FAILURE(Render(probe));
    UIElement* row = FindRow(parent, "Update Mode");
    ASSERT_NE(row, nullptr);
    Dropdown* modes = FindDropdown(*row);
    ASSERT_NE(modes, nullptr);
    EXPECT_EQ(modes->GetSelectedLabel(), "Realtime (on change)");
}
