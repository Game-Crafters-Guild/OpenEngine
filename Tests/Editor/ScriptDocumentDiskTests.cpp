#include "Panels/SaveSceneChangesModal.h"
#include "Panels/Script/ScriptEditorPanel.h"
#include "Panels/Script/ScriptHistoryDocument.h"
#include "Panels/Script/ScriptTextArea.h"
#include "Panels/Script/ScriptTextEditCommand.h"
#include "ScopedEditorUserDataRoot.h"
#include "TestTempDir.h"
#include "UndoRedo/UndoRedoService.h"

#include "AssetCore/SharedFileRead.h"
#include "Types/StringId.h"
#include "UI/UIManager.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <gtest/gtest.h>

namespace GameEngine::Editor
{
namespace
{
class ScriptDocumentDiskTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Root = TestUtils::MakeUniqueTempDirectory("ScriptDocumentDiskTests");
        std::filesystem::create_directories(m_Root);
        m_Preferences = std::make_unique<ScopedEditorUserDataRoot>(m_Root / "preferences");
        m_Path = m_Root / "script.txt";
        WriteDisk("original");
        OpenPanel();
        m_Document = ScriptHistoryDocument::Open(m_Path, "original");
    }

    void TearDown() override
    {
        m_Panel.reset();
        m_Preferences.reset();
        std::error_code error;
        std::filesystem::remove_all(m_Root, error);
    }

    void OpenPanel()
    {
        m_Panel = std::make_unique<ScriptEditorPanel>();
        m_Panel->SetUndoRedoService(&m_History);
        ASSERT_TRUE(m_Panel->OpenScript(m_Path));
        m_TextArea = dynamic_cast<ScriptTextArea*>(m_Panel->FindById(HashStringId("ScriptEditorTextArea")));
        ASSERT_NE(m_TextArea, nullptr);
    }

    void WriteDisk(const std::string& text)
    {
        std::ofstream file(m_Path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file.is_open());
        file << text;
        ASSERT_TRUE(file.good());
    }

    std::string ReadDisk() const
    {
        String text;
        EXPECT_TRUE(ReadFileTextShared(m_Path, text));
        return text;
    }

    void EditLocal()
    {
        const std::string before = m_Document->GetText();
        m_Document->SetText("local");
        m_History.CommitAlreadyApplied(std::make_unique<ScriptTextEditCommand>(m_Document, before, "local"));
        m_Document->Changed().Invoke();
    }

    SaveSceneChangesModal* ConflictModal() const
    {
        return dynamic_cast<SaveSceneChangesModal*>(m_Panel->FindById(HashStringId("ScriptDiskConflictModal")));
    }

    std::filesystem::path m_Root;
    std::filesystem::path m_Path;
    std::unique_ptr<ScopedEditorUserDataRoot> m_Preferences;
    UndoRedoService m_History;
    std::shared_ptr<ScriptHistoryDocument> m_Document;
    std::unique_ptr<ScriptEditorPanel> m_Panel;
    ScriptTextArea* m_TextArea = nullptr;
};

TEST_F(ScriptDocumentDiskTest, ReopeningASavedScriptLoadsExternalTextAndSavePreservesIt)
{
    EditLocal();
    ASSERT_TRUE(m_Panel->SaveScript());
    m_Panel.reset();
    WriteDisk("external");
    OpenPanel();
    EXPECT_EQ(m_TextArea->GetValue(), "external");
    EXPECT_FALSE(m_Document->IsDirty());
    ASSERT_TRUE(m_Panel->SaveScript());
    EXPECT_EQ(ReadDisk(), "external");
}

TEST_F(ScriptDocumentDiskTest, CancellingAConflictingSavePreservesBothVersions)
{
    EditLocal();
    WriteDisk("external");
    EXPECT_FALSE(m_Panel->SaveScript());
    EXPECT_EQ(ReadDisk(), "external");
    EXPECT_EQ(m_TextArea->GetValue(), "local");
    ASSERT_NE(ConflictModal(), nullptr);
    ASSERT_TRUE(ConflictModal()->IsVisible());
    EXPECT_NE(m_Panel->GetTitle().find("disk changed"), std::string::npos);
    ConflictModal()->ChooseCancel();
    EXPECT_EQ(ReadDisk(), "external");
    EXPECT_TRUE(m_Document->IsDirty());
    EXPECT_TRUE(m_Document->HasDiskConflict());
}

TEST_F(ScriptDocumentDiskTest, ReopeningADirtyScriptAllowsReloadingTheDiskVersion)
{
    EditLocal();
    m_Panel.reset();
    WriteDisk("external");
    OpenPanel();
    EXPECT_EQ(m_TextArea->GetValue(), "local");
    EXPECT_TRUE(m_Document->HasDiskConflict());
    ASSERT_FALSE(m_Panel->SaveScript());
    ASSERT_NE(ConflictModal(), nullptr);
    ConflictModal()->ChooseDontSave();
    EXPECT_EQ(m_TextArea->GetValue(), "external");
    EXPECT_EQ(ReadDisk(), "external");
    EXPECT_FALSE(m_Document->IsDirty());
    EXPECT_FALSE(m_Document->HasDiskConflict());
}

TEST_F(ScriptDocumentDiskTest, ConfirmedOverwriteSavesLocalText)
{
    EditLocal();
    WriteDisk("external");
    ASSERT_FALSE(m_Panel->SaveScript());
    ASSERT_NE(ConflictModal(), nullptr);
    ConflictModal()->ChooseSave();
    EXPECT_EQ(ReadDisk(), "local");
    EXPECT_FALSE(m_Document->IsDirty());
    EXPECT_FALSE(m_Document->HasDiskConflict());
    EXPECT_FALSE(ConflictModal()->IsVisible());
}

TEST_F(ScriptDocumentDiskTest, AnotherDiskChangeWhileThePromptIsOpenRequiresNewConfirmation)
{
    EditLocal();
    WriteDisk("external");
    ASSERT_FALSE(m_Panel->SaveScript());
    ASSERT_NE(ConflictModal(), nullptr);
    WriteDisk("newer external");
    ConflictModal()->ChooseSave();
    EXPECT_EQ(ReadDisk(), "newer external");
    ASSERT_TRUE(ConflictModal()->IsVisible());
    ConflictModal()->ChooseSave();
    EXPECT_EQ(ReadDisk(), "local");
}

TEST_F(ScriptDocumentDiskTest, AConflictPromptCannotSaveADifferentOpenScript)
{
    EditLocal();
    WriteDisk("external");
    ASSERT_FALSE(m_Panel->SaveScript());
    ASSERT_NE(ConflictModal(), nullptr);
    const auto otherPath = m_Root / "other.txt";
    {
        std::ofstream file(otherPath);
        file << "other";
    }
    ASSERT_TRUE(m_Panel->OpenScript(otherPath));
    ConflictModal()->ChooseSave();
    EXPECT_EQ(ReadDisk(), "external");
    EXPECT_EQ(m_TextArea->GetValue(), "other");
}

TEST_F(ScriptDocumentDiskTest, QuitWaitsForAConflictingSaveToBeConfirmed)
{
    EditLocal();
    WriteDisk("external");
    bool proceeded = false;
    m_Panel->PromptSaveBeforeQuit([&proceeded] { proceeded = true; });
    SaveSceneChangesModal* quitModal = nullptr;
    for (const auto& child : m_Panel->GetChildren())
        if (auto* modal = dynamic_cast<SaveSceneChangesModal*>(child.get()))
            quitModal = modal;
    ASSERT_NE(quitModal, nullptr);
    quitModal->ChooseSave();
    EXPECT_FALSE(proceeded);
    EXPECT_EQ(ReadDisk(), "external");
    ASSERT_NE(ConflictModal(), nullptr);
    ConflictModal()->ChooseSave();
    EXPECT_TRUE(proceeded);
    EXPECT_EQ(ReadDisk(), "local");
}

TEST_F(ScriptDocumentDiskTest, ClosingThePanelDetachesItsWindowLevelConflictPrompt)
{
    EditLocal();
    WriteDisk("external");
    UIManager manager{nullptr};
    auto root = std::make_unique<UIElement>();
    UIElement* rootElement = root.get();
    ScriptEditorPanel* panel = m_Panel.get();
    root->AddChild(std::move(m_Panel));
    manager.SetRoot(std::move(root));
    ASSERT_EQ(panel->GetOwnerManager(), &manager);
    ASSERT_FALSE(panel->SaveScript());
    auto* modal = dynamic_cast<SaveSceneChangesModal*>(rootElement->FindById(HashStringId("ScriptDiskConflictModal")));
    ASSERT_NE(modal, nullptr);
    const auto weakModal = UIElement::MakeWeakRef(modal);
    rootElement->RemoveChild(panel);
    EXPECT_FALSE(modal->IsVisible());
    modal->ChooseSave();
    EXPECT_EQ(ReadDisk(), "external");
    manager.DrainDeferredActionsOnce();
    EXPECT_EQ(weakModal.Get(), nullptr);
}
} // namespace
} // namespace GameEngine::Editor
