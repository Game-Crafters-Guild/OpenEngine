#include "Panels/GraphPanel.h"

#include "Graph/GraphModel.h"
#include "Graph/GraphAsset.h"
#include "Platform/Shell.h"
#include "TestTempDir.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "UndoRedo/UndoRedoService.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#include <gtest/gtest.h>

using namespace GameEngine;

namespace
{
std::filesystem::path s_SavePath;
std::uint32_t s_SaveDialogCalls = 0;

Graph::Model MakeModel(const std::string& nodeId, const std::string& value)
{
    Graph::Model model;
    model.KindId = Graph::kKindIdGameLogic;
    Graph::Node node;
    node.Id = nodeId;
    node.TypeId = "HistoryTestNode";
    node.Parameters["value"] = value;
    model.Nodes.push_back(std::move(node));
    Graph::Variable variable;
    variable.Name = "Parameter";
    variable.Type = "float";
    variable.Value = "0";
    model.Variables.push_back(std::move(variable));
    return model;
}

bool BuildHistorySample(Graph::Model& model)
{
    model = MakeModel("sample", "0");
    return true;
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

class HistoryGraphPanel final : public GraphPanel
{
public:
    HistoryGraphPanel() : GraphPanel(Graph::kKindIdGameLogic)
    {
        GraphKindHooks hooks;
        hooks.Sample.Label = "History Sample";
        hooks.Sample.Build = BuildHistorySample;
        SetKindHooks(std::move(hooks));
        SetupUI();
        RootModel() = MakeModel("node", "0");
        Canvas()->SetModel(&RootModel());
    }

    std::string Value(const std::string& nodeId = "node") const
    {
        const Graph::Node* node = ActiveGraphModel()->FindNode(nodeId);
        return node ? node->Parameters.GetString("value", "") : "missing";
    }

    void LoadSample()
    {
        Label* label = FindLabel(*this, "History Sample");
        ASSERT_NE(label, nullptr);
        UIElement* parent = label;
        while (parent && !dynamic_cast<Button*>(parent))
            parent = parent->GetParent();
        ASSERT_NE(parent, nullptr);
        UIEvent click{};
        click.Id = kEventButtonClick;
        parent->DispatchEvent(click);
        GetOwnerManager()->DrainDeferredActionsOnce();
    }
};

class GraphPanelHistoryTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        s_SavePath = Directory.Path() / "first.graph";
        s_SaveDialogCalls = 0;
        Manager.SetRoot(std::make_unique<UIElement>());
        auto panel = std::make_unique<HistoryGraphPanel>();
        Panel = panel.get();
        Panel->SetUndoRedoService(&History);
        Manager.GetRootElement()->AddChild(std::move(panel));
    }

    void TearDown() override
    {
        Manager.DrainDeferredActionsOnce();
    }

    TestUtils::ScopedTempDir Directory{TestUtils::MakeUniqueTempDirectory("GraphPanelHistoryTests")};
    Editor::UndoRedoService History;
    UIManager Manager{nullptr};
    HistoryGraphPanel* Panel = nullptr;
};

TEST_F(GraphPanelHistoryTests, DiscreteCommandSurvivesTheFirstSave)
{
    ASSERT_TRUE(Panel->GetCurrentGraphPath().empty());
    ASSERT_TRUE(Panel->SetNodeParameter("node", "value", "1"));
    ASSERT_TRUE(Panel->SaveGraph());
    ASSERT_FALSE(Panel->GetCurrentGraphPath().empty());
    ASSERT_TRUE(std::filesystem::exists(Panel->GetCurrentGraphPath()));
    EXPECT_EQ(s_SaveDialogCalls, 1u);
    History.Undo();
    EXPECT_EQ(Panel->Value(), "0");
    EXPECT_TRUE(Panel->HasUnsavedChanges());
    History.Redo();
    EXPECT_EQ(Panel->Value(), "1");
    EXPECT_TRUE(Panel->SaveGraph());
    EXPECT_EQ(s_SaveDialogCalls, 1u);
}

TEST_F(GraphPanelHistoryTests, InteractiveParameterSnapshotSurvivesTheFirstSave)
{
    ASSERT_TRUE(Panel->SetNodeParameter("node", "value", "1", false));
    ASSERT_TRUE(Panel->SaveGraph());
    History.Undo();
    EXPECT_EQ(Panel->Value(), "0");
    History.Redo();
    EXPECT_EQ(Panel->Value(), "1");
    ASSERT_TRUE(Panel->SetNodeParameter("node", "value", "2", false));
    ASSERT_TRUE(Panel->SaveGraph());
    History.Undo();
    EXPECT_EQ(Panel->Value(), "1");
    History.Undo();
    EXPECT_EQ(Panel->Value(), "0");
}

TEST_F(GraphPanelHistoryTests, InteractiveVariableSnapshotSurvivesTheFirstSave)
{
    ASSERT_TRUE(Panel->EditGraphVariableFromInspector("Parameter", "2", false));
    ASSERT_TRUE(Panel->SaveGraph());
    History.Undo();
    EXPECT_EQ(Panel->ActiveGraphModel()->Variables.front().Value, "0");
    History.Redo();
    EXPECT_EQ(Panel->ActiveGraphModel()->Variables.front().Value, "2");
}

TEST_F(GraphPanelHistoryTests, UnsavedSampleCommandsSurviveTheFirstSave)
{
    Panel->LoadSample();
    ASSERT_TRUE(Panel->GetCurrentGraphPath().empty());
    ASSERT_EQ(Panel->Value("sample"), "0");
    ASSERT_TRUE(Panel->SetNodeParameter("sample", "value", "3"));
    ASSERT_TRUE(Panel->SaveGraph());
    History.Undo();
    EXPECT_EQ(Panel->Value("sample"), "0");
    History.Redo();
    EXPECT_EQ(Panel->Value("sample"), "3");
}

TEST_F(GraphPanelHistoryTests, ReplacingAnUnsavedDocumentRejectsItsOldCommands)
{
    ASSERT_TRUE(Panel->SetNodeParameter("node", "value", "1"));
    Panel->LoadSample();
    ASSERT_EQ(Panel->Value("sample"), "0");
    History.Undo();
    EXPECT_EQ(Panel->Value("sample"), "0");
    EXPECT_EQ(Panel->Value(), "missing");
    EXPECT_FALSE(History.GetLastError().empty());
}

TEST_F(GraphPanelHistoryTests, ReloadingTheSamePathRejectsThePreviousDocumentHistory)
{
    ASSERT_TRUE(Panel->SetNodeParameter("node", "value", "1"));
    ASSERT_TRUE(Panel->SaveGraph());
    const auto path = Panel->GetCurrentGraphPath();
    ASSERT_TRUE(Panel->SetNodeParameter("node", "value", "2"));
    ASSERT_TRUE(SaveGraphToPath(MakeModel("node", "9"), path));
    ASSERT_TRUE(Panel->OpenGraph(path));
    History.Undo();
    EXPECT_EQ(Panel->Value(), "9");
    EXPECT_FALSE(History.GetLastError().empty());
}

TEST_F(GraphPanelHistoryTests, FailedOpenKeepsTheCurrentHistoryValid)
{
    ASSERT_TRUE(Panel->SetNodeParameter("node", "value", "1"));
    EXPECT_FALSE(Panel->OpenGraph(Directory.Path() / "missing.graph"));
    History.Undo();
    EXPECT_EQ(Panel->Value(), "0");
    EXPECT_TRUE(History.GetLastError().empty());
}

TEST_F(GraphPanelHistoryTests, ReplacingAnUnsavedDocumentSettlesAndRejectsItsInteractiveEdit)
{
    ASSERT_TRUE(Panel->SetNodeParameter("node", "value", "1", false));
    Panel->LoadSample();
    ASSERT_EQ(Panel->Value("sample"), "0");
    EXPECT_FALSE(History.HasPendingEdit());
    History.Undo();
    EXPECT_EQ(Panel->Value("sample"), "0");
    EXPECT_FALSE(History.GetLastError().empty());
    ASSERT_TRUE(Panel->SetNodeParameter("sample", "value", "2", false));
    ASSERT_TRUE(Panel->SaveGraph());
    History.Undo();
    EXPECT_EQ(Panel->Value("sample"), "0");
    EXPECT_TRUE(History.GetLastError().empty());
}

TEST_F(GraphPanelHistoryTests, CancelingTheFirstSaveKeepsTheCurrentHistoryValid)
{
    ASSERT_TRUE(Panel->SetNodeParameter("node", "value", "1", false));
    s_SavePath.clear();
    EXPECT_FALSE(Panel->SaveGraph());
    EXPECT_TRUE(Panel->GetCurrentGraphPath().empty());
    EXPECT_EQ(s_SaveDialogCalls, 1u);
    History.Undo();
    EXPECT_EQ(Panel->Value(), "0");
    EXPECT_TRUE(History.GetLastError().empty());
}
} // namespace

namespace GameEngine::Platform
{
// The test chooses a destination while GraphPanel performs its normal save and history work.
std::filesystem::path SaveFile(const std::filesystem::path&, const char*, const char*)
{
    ++s_SaveDialogCalls;
    return std::exchange(s_SavePath, {});
}
} // namespace GameEngine::Platform
