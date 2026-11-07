#include "InspectorRegistry.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Inspectors/LensFlareSourceInspector.h"
#include "Scene/SceneDocumentManager.h"

#include "AssetCore/SharedFileRead.h"
#include "Assets/LensFlareDefinitionAsset.h"
#include "Core/Engine.h"
#include "ECS/World.h"
#include "Scripting/ScriptsConfig.h"
#include "TestTempDir.h"
#include "UI/Controls/Label.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UndoRedo/UndoRedoService.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#include <gtest/gtest.h>

namespace GameEngine
{
namespace
{
constexpr std::uint32_t kRed = 0xFFFF0000u;
constexpr std::uint32_t kGreen = 0xFF00FF00u;
constexpr std::uint32_t kBlue = 0xFF0000FFu;
constexpr float kIntensity = 3.0f;

enum class TintRow
{
    Global,
    Element
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

Label* FindColorValue(UIElement& row)
{
    for (const auto& field : row.GetChildren())
        if (field->HasClass("inspector-field"))
            for (const auto& control : field->GetChildren())
                if (auto* label = dynamic_cast<Label*>(control.get()); label && label->HasClass("inspector-text"))
                    return label;
    return nullptr;
}

class LensFlareColorPickerTests : public ::testing::TestWithParam<TintRow>
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
        m_Flare.EditElements().emplace_back();
        ASSERT_TRUE(m_Flare.Save());
        ASSERT_TRUE(m_Flare.Load());
        m_Scene.SetWorld(&m_World);
        m_Scene.SetUndoRedoService(&m_Undo);
        m_Scene.MarkClean();
        RegisterLensFlareSourceInspector();
        BuildInspector(true);
    }

    void TearDown() override
    {
        m_Callbacks.reset();
        std::filesystem::permissions(m_Flare.GetPath(), std::filesystem::perms::owner_all);
    }

    void BuildInspector(bool withUndo)
    {
        m_Root.RemoveAllChildren();
        InspectorContext context{};
        context.Parent = &m_Root;
        context.Object = &m_Flare;
        context.Undo = withUndo ? &m_Undo : nullptr;
        context.OpenColorPickerWindow = [this](std::uint32_t argb, float intensity, ColorPickerCallbacks callbacks)
        {
            m_OpenedArgb = argb;
            m_OpenedIntensity = intensity;
            m_Callbacks = std::move(callbacks);
        };
        InspectorFn* inspector = InspectorRegistry::Get().TryGetAssetInspector(AssetType::LensFlareDefinition);
        ASSERT_NE(inspector, nullptr);
        (*inspector)(context);
    }

    void OpenPicker(std::uint32_t expectedArgb = 0xFFFFFFFFu, float expectedIntensity = 1.0f)
    {
        Label* name = FindLabel(m_Root, GetParam() == TintRow::Global ? "Global Tint" : "Tint");
        ASSERT_NE(name, nullptr);
        UIElement* row = InspectorUI::RowOfControl(name);
        ASSERT_NE(row, nullptr);
        Label* value = FindColorValue(*row);
        ASSERT_NE(value, nullptr);
        UIEvent press{};
        press.Id = kEventMouseDown;
        press.Button = 0;
        value->DispatchEvent(press);
        ASSERT_TRUE(m_Callbacks);
        EXPECT_EQ(m_OpenedArgb, expectedArgb);
        EXPECT_FLOAT_EQ(m_OpenedIntensity, expectedIntensity);
    }

    LensFlare::Color ColorOf(const LensFlareDefinitionAsset& asset) const
    {
        return GetParam() == TintRow::Global ? asset.GetGlobals().GlobalTint : asset.GetElements().at(0).Tint;
    }

    void ExpectColor(float red, float green, float blue, bool disk = false)
    {
        LensFlare::Color color;
        if (disk)
        {
            LensFlareDefinitionAsset stored(GUID::Null(), m_Flare.GetPath());
            ASSERT_TRUE(stored.Load());
            color = ColorOf(stored);
        }
        else
            color = ColorOf(m_Flare);
        EXPECT_FLOAT_EQ(color.R, red);
        EXPECT_FLOAT_EQ(color.G, green);
        EXPECT_FLOAT_EQ(color.B, blue);
        EXPECT_FLOAT_EQ(color.A, 1.0f);
    }

    void SaveScene()
    {
        ASSERT_TRUE(m_Scene.SaveAs(m_Directory.Path() / "Picker.scene"));
        EXPECT_FALSE(m_Undo.HasOpenInteractiveEdit());
        EXPECT_FALSE(m_Scene.IsDirty());
    }

    std::string ReadFile() const
    {
        String text;
        EXPECT_TRUE(ReadFileTextShared(m_Flare.GetPath(), text));
        return text;
    }

    TestUtils::ScopedTempDir m_Directory{TestUtils::MakeUniqueTempDirectory("LensFlareColorPickerTests")};
    LensFlareDefinitionAsset m_Flare{GUID::Null(), m_Directory.Path() / "Picker.lensflare"};
    ECS::World m_World;
    Editor::UndoRedoService m_Undo;
    Editor::SceneDocumentManager m_Scene;
    UIElement m_Root;
    std::optional<ColorPickerCallbacks> m_Callbacks;
    std::uint32_t m_OpenedArgb = 0;
    float m_OpenedIntensity = 0.0f;
};

TEST_P(LensFlareColorPickerTests, ApplyResumesAfterRepeatedSceneSavesAndInspectorRebuild)
{
    OpenPicker();
    const std::string original = ReadFile();
    m_Callbacks->onValueChanging(kRed, kIntensity);
    ExpectColor(kIntensity, 0.0f, 0.0f);
    SaveScene();
    EXPECT_EQ(ReadFile(), original);
    m_Callbacks->onValueChanging(kGreen, kIntensity);
    SaveScene();
    BuildInspector(true);
    m_Callbacks->onApply(kBlue, kIntensity);
    ExpectColor(0.0f, 0.0f, kIntensity, true);
    m_Undo.Undo();
    ExpectColor(1.0f, 1.0f, 1.0f);
    EXPECT_EQ(ReadFile(), original);
    m_Undo.Redo();
    ExpectColor(0.0f, 0.0f, kIntensity);
    ExpectColor(0.0f, 0.0f, kIntensity, true);
}

TEST_P(LensFlareColorPickerTests, CancelAfterSceneSaveRestoresThePersistedColor)
{
    OpenPicker();
    const std::string original = ReadFile();
    m_Callbacks->onValueChanging(kRed, kIntensity);
    SaveScene();
    m_Callbacks->onCancel();
    ExpectColor(1.0f, 1.0f, 1.0f);
    EXPECT_EQ(ReadFile(), original);
    EXPECT_FALSE(m_Undo.CanUndo());
}

TEST_P(LensFlareColorPickerTests, CancelAfterResumedPreviewPreservesLaterDiskFields)
{
    OpenPicker();
    m_Callbacks->onValueChanging(kRed, kIntensity);
    SaveScene();
    LensFlareDefinitionAsset otherEditor(GUID::Null(), m_Flare.GetPath());
    ASSERT_TRUE(otherEditor.Load());
    otherEditor.EditGlobals().GlobalBrightness = 2.5f;
    ASSERT_TRUE(otherEditor.Save());
    const std::string saved = ReadFile();
    m_Callbacks->onValueChanging(kGreen, kIntensity);
    m_Callbacks->onCancel();
    ExpectColor(1.0f, 1.0f, 1.0f);
    EXPECT_FLOAT_EQ(m_Flare.GetGlobals().GlobalBrightness, 2.5f);
    EXPECT_EQ(ReadFile(), saved);
}

TEST_P(LensFlareColorPickerTests, ApplyWorksAfterSavingAnUnchangedOpenPicker)
{
    OpenPicker();
    SaveScene();
    m_Callbacks->onApply(kGreen, kIntensity);
    m_Undo.Undo();
    ExpectColor(1.0f, 1.0f, 1.0f);
    m_Undo.Redo();
    ExpectColor(0.0f, kIntensity, 0.0f, true);
}

TEST_P(LensFlareColorPickerTests, CancelWithoutHistoryRestoresOnlyTheOpeningColor)
{
    BuildInspector(false);
    OpenPicker();
    const std::string original = ReadFile();
    m_Callbacks->onValueChanging(kRed, kIntensity);
    m_Flare.EditGlobals().GlobalBrightness = 2.5f;
    m_Callbacks->onCancel();
    ExpectColor(1.0f, 1.0f, 1.0f);
    EXPECT_FLOAT_EQ(m_Flare.GetGlobals().GlobalBrightness, 2.5f);
    EXPECT_EQ(ReadFile(), original);
}

TEST_P(LensFlareColorPickerTests, ApplyWithoutHistoryPersistsThePreview)
{
    BuildInspector(false);
    OpenPicker();
    m_Callbacks->onValueChanging(kRed, kIntensity);
    m_Callbacks->onApply(kBlue, kIntensity);
    ExpectColor(0.0f, 0.0f, kIntensity, true);
    EXPECT_FALSE(m_Undo.CanUndo());
}

TEST_P(LensFlareColorPickerTests, CancelingAnUnchangedReopenedPickerPreservesThePreviousApply)
{
    OpenPicker();
    m_Callbacks->onApply(kRed, kIntensity);
    BuildInspector(true);
    OpenPicker(kRed, kIntensity);
    m_Callbacks->onCancel();
    ExpectColor(kIntensity, 0.0f, 0.0f);
    ExpectColor(kIntensity, 0.0f, 0.0f, true);
    m_Undo.Undo();
    ExpectColor(1.0f, 1.0f, 1.0f);
    m_Undo.Redo();
    ExpectColor(kIntensity, 0.0f, 0.0f, true);
}

#if defined(_WIN32)
TEST_P(LensFlareColorPickerTests, RefusedFileSaveRestoresThePersistedColorWithoutHistory)
{
    OpenPicker();
    const std::string original = ReadFile();
    m_Callbacks->onValueChanging(kRed, kIntensity);
    std::filesystem::permissions(m_Flare.GetPath(), std::filesystem::perms::owner_read);
    m_Callbacks->onApply(kBlue, kIntensity);
    ExpectColor(1.0f, 1.0f, 1.0f);
    EXPECT_EQ(ReadFile(), original);
    EXPECT_FALSE(m_Undo.CanUndo());
}
#endif

std::string TintRowName(const ::testing::TestParamInfo<TintRow>& info)
{
    return info.param == TintRow::Global ? "GlobalTint" : "ElementTint";
}

INSTANTIATE_TEST_SUITE_P(AssetRows, LensFlareColorPickerTests,
    ::testing::Values(TintRow::Global, TintRow::Element), TintRowName);
} // namespace
} // namespace GameEngine
