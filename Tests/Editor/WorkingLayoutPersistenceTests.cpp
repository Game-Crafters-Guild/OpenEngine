// The working dock layout survives a save and a restore, per user and per
// project, and falls back to the active preset when the saved tree cannot be
// used.
#include "Docking/LayoutPresetController.h"
#include "Docking/WorkingLayoutPersistence.h"

#include "Editor/Registries/EditorPanelRegistry.h"
#include "Editor/Settings/SettingsStore.h"
#include "EditorDockNodeJson.h"
#include "Logger/CallbackSink.h"
#include "Logger/Logger.h"
#include "TestEnvVar.h"
#include "TestTempDir.h"
#include "UI/Layout/Docking.h"
#include "UI/UIElement.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace
{
using namespace GameEngine;
using Editor::LayoutPresetController;
using Editor::WorkingLayoutPersistence;

class WorkingLayoutLogCounter
{
  public:
    explicit WorkingLayoutLogCounter(std::string needle) : m_Needle(std::move(needle))
    {
        Logger::Log::Initialize({Logger::LogLevel::Debug, false});
        auto sink = std::make_unique<Logger::CallbackSink>();
        m_Sink = sink.get();
        Logger::Log::AddSink(std::move(sink));
        m_CallbackId = m_Sink->RegisterCallback([this](const Logger::LogMessage& message) {
            if (message.Message.find(m_Needle) != std::string::npos)
                ++m_Count;
        });
    }
    ~WorkingLayoutLogCounter() { m_Sink->UnregisterCallback(m_CallbackId); }

    int Count() const
    {
        Logger::Log::Flush();
        return m_Count.load();
    }

  private:
    std::string m_Needle;
    Logger::CallbackSink* m_Sink = nullptr;
    std::uint64_t m_CallbackId = 0;
    std::atomic<int> m_Count{0};
};

// [ Hierarchy | [ SceneView / Assets, Log ] ]
std::unique_ptr<DockNode> MakePresetLayout()
{
    auto bottom = DockNode::MakeLeaf();
    bottom->AddTab("Assets");
    bottom->AddTab("Log");
    auto top = DockNode::MakeLeaf();
    top->AddTab("SceneView");
    auto workspace = std::make_unique<DockNode>();
    workspace->SetSplit(DockPosition::Top, 0.75f, std::move(top), std::move(bottom));
    auto left = DockNode::MakeLeaf();
    left->AddTab("Hierarchy");
    auto root = std::make_unique<DockNode>();
    root->SetSplit(DockPosition::Left, 0.25f, std::move(left), std::move(workspace));
    return root;
}

class WorkingLayoutPersistenceTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        // The user's project settings go under a scratch user-data root, never the
        // developer's own profile.
        Testing::SetEnvVar("GE_EDITOR_USER_DATA_ROOT", (m_Root.Path() / "UserData").string().c_str());
        m_Project = m_Root.Path() / "Project";
        std::filesystem::create_directories(m_Project);
    }

    void TearDown() override { Testing::SetEnvVar("GE_EDITOR_USER_DATA_ROOT", ""); }

    // A dock with every panel the preset names, showing the preset.
    std::unique_ptr<DockingManager> MakeDocking(const std::vector<std::string>& extraPanels = {})
    {
        auto docking = std::make_unique<DockingManager>();
        for (const char* id : {"Hierarchy", "SceneView", "Assets", "Log"})
            docking->RegisterPanel(id, &m_PanelContent);
        for (const std::string& id : extraPanels)
            docking->RegisterPanel(id, &m_PanelContent);
        docking->SetRoot(MakePresetLayout());
        return docking;
    }

    TestUtils::ScopedTempDir m_Root{TestUtils::MakeUniqueTempDirectory("WorkingLayout")};
    std::filesystem::path m_Project;
    UIElement m_PanelContent;
};

TEST_F(WorkingLayoutPersistenceTest, AMovedTabSurvivesASaveAndARestore)
{
    auto session = MakeDocking();
    ASSERT_TRUE(session->RemoveTabForDockMove("Log"));
    session->GetRoot()->First()->AddTab("Log");
    const nlohmann::json moved = DockNodeToJson(session->GetRoot());
    WorkingLayoutPersistence::Save(m_Project, session->GetRoot(), *session, {});

    auto nextSession = MakeDocking();
    std::unique_ptr<DockNode> restored = WorkingLayoutPersistence::Load(m_Project, *nextSession);

    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(DockNodeToJson(restored.get()), moved);
}

TEST_F(WorkingLayoutPersistenceTest, ASavedLayoutNamingAnUnknownPanelFallsBackToThePreset)
{
    auto session = MakeDocking({"RetiredPanel"});
    session->GetRoot()->First()->AddTab("RetiredPanel");
    WorkingLayoutPersistence::Save(m_Project, session->GetRoot(), *session, {});

    WorkingLayoutLogCounter fallback("names panels this editor does not have (RetiredPanel)");
    auto nextSession = MakeDocking();

    EXPECT_EQ(WorkingLayoutPersistence::Load(m_Project, *nextSession), nullptr);
    EXPECT_EQ(fallback.Count(), 1);
}

TEST_F(WorkingLayoutPersistenceTest, ARecordThatIsNotAValidLayoutFallsBackToThePreset)
{
    const std::filesystem::path file = Editor::OpenUserProjectSettings(m_Project).GetFilePath();
    std::filesystem::create_directories(file.parent_path());
    const std::vector<std::string> records = {
        R"({"ui.workingLayout": "Hierarchy"})",
        R"({"ui.workingLayout": {"layout": "Hierarchy"}})",
        R"({"ui.workingLayout": {"layout": ["Hierarchy"]}})",
        R"({"ui.workingLayout": {"layout": {"kind": "bogus"}}})",
        R"({"ui.workingLayout": {"layout": {"kind": "split", "dir": "Left",
            "first": {"kind": "leaf", "tabs": ["Hierarchy"]}}}})",
        R"({"ui.workingLayout": {"layout": {"kind": "split", "dir": "Left",
            "first": {"kind": "leaf", "tabs": ["Hierarchy", "Log"]},
            "second": {"kind": "leaf", "tabs": ["Log"]}}}})",
        R"({"ui.workingLayout": {"layout": {"kind": "leaf", "tabs": ["Hier)",
    };
    auto nextSession = MakeDocking();
    for (const std::string& record : records)
    {
        std::ofstream(file, std::ios::trunc) << record;
        WorkingLayoutLogCounter fallback("using the active layout preset");

        EXPECT_EQ(WorkingLayoutPersistence::Load(m_Project, *nextSession), nullptr) << record;
        EXPECT_EQ(fallback.Count(), 1) << record;
    }
}

TEST_F(WorkingLayoutPersistenceTest, APackagePanelRegisteredAfterTheRestoreSitsWhereItWasSaved)
{
    Editor::EditorPanelDescriptor descriptor;
    descriptor.PanelId = "WorkingLayoutPackagePanel";
    descriptor.Factory = [] { return std::unique_ptr<UIElement>(); };
    Editor::EditorPanelRegistry::Get().RegisterPanel(std::move(descriptor));
    auto session = MakeDocking({"WorkingLayoutPackagePanel"});
    session->GetRoot()->First()->AddTab("WorkingLayoutPackagePanel");
    const nlohmann::json docked = DockNodeToJson(session->GetRoot());
    WorkingLayoutPersistence::Save(m_Project, session->GetRoot(), *session, {});

    auto nextSession = MakeDocking();
    std::unique_ptr<DockNode> restored = WorkingLayoutPersistence::Load(m_Project, *nextSession);
    ASSERT_NE(restored, nullptr);
    nextSession->SetRoot(std::move(restored));
    nextSession->RegisterPanel("WorkingLayoutPackagePanel", &m_PanelContent);

    EXPECT_EQ(DockNodeToJson(nextSession->GetRoot()), docked);
}

TEST_F(WorkingLayoutPersistenceTest, APlaceholderWhosePackageNeverRegisteredIsNotSaved)
{
    auto session = MakeDocking();
    session->GetRoot()->First()->AddTab("WorkingLayoutUninstalledPackagePanel");
    WorkingLayoutPersistence::Save(m_Project, session->GetRoot(), *session, {});

    auto nextSession = MakeDocking();
    std::unique_ptr<DockNode> restored = WorkingLayoutPersistence::Load(m_Project, *nextSession);

    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(DockNodeToJson(restored.get()), DockNodeToJson(MakePresetLayout().get()));
}

TEST_F(WorkingLayoutPersistenceTest, ARestoredLayoutThatDiffersFromThePresetIsDirty)
{
    auto session = MakeDocking();
    ASSERT_TRUE(session->RemoveTabForDockMove("Log"));
    session->GetRoot()->First()->AddTab("Log");
    WorkingLayoutPersistence::Save(m_Project, session->GetRoot(), *session, {});

    auto nextSession = MakeDocking();
    const std::unique_ptr<DockNode> preset = MakePresetLayout();
    ASSERT_FALSE(LayoutPresetController::IsCurrentLayoutDirty(*nextSession, nullptr, preset.get()));
    std::unique_ptr<DockNode> restored = WorkingLayoutPersistence::Load(m_Project, *nextSession);
    ASSERT_NE(restored, nullptr);
    ASSERT_TRUE(LayoutPresetController::Apply(*nextSession, nullptr, restored.get(), false));

    EXPECT_TRUE(LayoutPresetController::IsCurrentLayoutDirty(*nextSession, nullptr, preset.get()));
}

} // namespace
