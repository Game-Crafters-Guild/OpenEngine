#include "AssetCore/AssetEvents.h"
#include "AssetCore/SharedFileRead.h"
#include "Assets/AssetManager.h"
#include "Core/Engine.h"
#include "Editor/EditorPaths.h"
#include "Logger/CallbackSink.h"
#include "Logger/Logger.h"
#include "Types/StringUtils.h"
#include "UI/ControlLayoutLoader.h"
#include "UI/UIElement.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <ostream>
#include <regex>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace GameEngine
{
namespace
{

// A control's layout file and the ids its constructor requires (its kRequiredIds).
struct ControlLayout
{
    const char* Name;
    const char* LayoutAssetPath;
    std::vector<std::string_view> RequiredIds;
};

// Names the case in a failure message.
void PrintTo(const ControlLayout& control, std::ostream* stream)
{
    *stream << control.Name;
}

const std::array<ControlLayout, 42> kControlLayouts = {{
    {"ConfirmActionModal",
     "UI/controls/ConfirmActionModal/ConfirmActionModal.uxml",
     {"confirm-action-title", "confirm-action-message", "confirm-action-detail", "confirm-action-closing",
      "confirm-action-cancel", "confirm-action-confirm"}},
    {"SaveSceneChangesModal",
     "UI/controls/SaveSceneChangesModal/SaveSceneChangesModal.uxml",
     {"save-changes-title", "save-changes-message", "save-changes-cancel", "save-changes-dont-save",
      "save-changes-save"}},
    {"RestoreSceneBackupModal",
     "UI/controls/RestoreSceneBackupModal/RestoreSceneBackupModal.uxml",
     {"restore-backup-title", "restore-backup-message", "restore-backup-discard", "restore-backup-restore"}},
    {"RenameLayoutModal",
     "UI/controls/RenameLayoutModal/RenameLayoutModal.uxml",
     {"rename-layout-title", "rename-layout-name", "rename-layout-cancel", "rename-layout-rename"}},
    {"DownloadInProgressModal",
     "UI/controls/DownloadInProgressModal/DownloadInProgressModal.uxml",
     {"download-progress-title", "download-progress-message", "download-progress-cancel",
      "download-progress-proceed", "download-progress-wait"}},
    {"PlayModeEntryModal",
     "UI/controls/PlayModeEntryModal/PlayModeEntryModal.uxml",
     {"play-entry-title", "play-entry-message", "play-entry-activity", "play-entry-progress-track",
      "play-entry-progress-fill", "play-entry-button"}},
    {"TransientNotice", "UI/controls/TransientNotice/TransientNotice.uxml", {"TransientNoticeText"}},
    {"LogPanel",
     "UI/panels/LogPanel/LogPanel.uxml",
     {"LogFilterError", "LogFilterWarning", "LogFilterInfo", "LogFilterDebug", "LogChannels", "LogClear",
      "LogPause", "LogCopyAll", "LogSearchHost", "LogSplit", "LogListPane", "LogCopyNotice", "LogDetailSplitter",
      "LogDetailPane", "LogDetailGlyph", "LogDetailSeverity", "LogDetailChannel", "LogDetailTime",
      "LogDetailSource", "LogDetailScroll", "LogDetailBody", "LogDetailMessage", "LogDetailTraceTitle",
      "LogDetailFrames"}},
    {"HierarchyNavigationBar", "UI/panels/HierarchyNavigationBar/HierarchyNavigationBar.uxml", {}},
    {"FolderBakeStatus", "UI/thumbnails/FolderBakeStatus/FolderBakeStatus.uxml", {}},
    {"CameraCustomAspectModal",
     "UI/controls/CameraCustomAspectModal/CameraCustomAspectModal.uxml",
     {"camera-aspect-width", "camera-aspect-height", "camera-aspect-cancel", "camera-aspect-apply"}},
    {"PlayModeChangeReviewModal",
     "UI/controls/PlayModeChangeReviewModal/PlayModeChangeReviewModal.uxml",
     {"pmcr-scroll", "pmcr-discard", "pmcr-apply"}},
    {"PlayModeChangeReviewRow", "UI/controls/PlayModeChangeReviewModal/PlayModeChangeReviewRow.uxml", {}},
    {"SaveSceneAsModal",
     "UI/controls/SaveSceneAsModal/SaveSceneAsModal.uxml",
     {"save-scene-as-name", "save-scene-as-browse", "save-scene-as-directory", "save-scene-as-folders",
      "save-scene-as-warning", "save-scene-as-cancel", "save-scene-as-save"}},
    {"SaveSceneAsFolderRow", "UI/controls/SaveSceneAsModal/SaveSceneAsFolderRow.uxml", {}},
    {"AddTagModal", "UI/controls/AddTagModal/AddTagModal.uxml", {"add-tag-window", "add-tag-header", "add-tag-scroll"}},
    {"AddTagModalList",
     "UI/controls/AddTagModal/AddTagModalList.uxml",
     {"add-tag-all-row", "add-tag-new-section", "add-tag-new-name", "add-tag-new-add"}},
    {"AddTagModalTagRow", "UI/controls/AddTagModal/AddTagModalTagRow.uxml", {}},
    {"VideoPlayerModal",
     "UI/controls/VideoPlayerModal/VideoPlayerModal.uxml",
     {"vpm-window", "vpm-header", "vpm-title", "vpm-close", "vpm-video-area", "vpm-seek-bar", "vpm-seek-fill",
      "vpm-time", "vpm-play-pause", "vpm-stop", "vpm-fullscreen"}},
    {"InspectorNotice", "UI/controls/InspectorNotice/InspectorNotice.uxml", {}},
    {"ListItemActions", "UI/controls/ListItemActions/ListItemActions.uxml", {}},
    {"PictureChoiceField", "UI/controls/PictureChoiceField/PictureChoiceField.uxml", {}},
    {"SplineFencePartRow", "UI/inspectors/SplineFencePartRow/SplineFencePartRow.uxml", {}},
    {"SplineFenceOverrideRow", "UI/inspectors/SplineFenceOverrideRow/SplineFenceOverrideRow.uxml", {}},
    {"EntityHeader",
     "UI/inspectors/EntityHeader/EntityHeader.uxml",
     {"inspector-header-badge", "inspector-header-history", "inspector-history-back", "inspector-history-forward"}},
    {"ScriptPackagesList", "UI/controls/ScriptPackagesList/ScriptPackagesList.uxml", {}},
    {"ScriptPackageRow", "UI/controls/ScriptPackagesList/ScriptPackageRow.uxml", {}},
    {"LogRow", "UI/panels/LogPanel/LogRow.uxml", {"LogRowGlyph", "LogRowTime", "LogRowChannel", "LogRowMessage"}},
    {"LogChannelPopover",
     "UI/panels/LogPanel/LogChannelPopover.uxml",
     {"LogChannelsScroll", "LogChannelsSearchHost", "LogChannelsColumns", "LogChannelsShowAll", "LogChannelsHideAll",
      "LogChannelsCount"}},
    {"LogChannelRow", "UI/panels/LogPanel/LogChannelRow.uxml", {"LogChannelShown", "LogChannelName", "LogChannelLevel"}},
    {"VcsCommitDialog",
     "UI/controls/VcsCommitDialog/VcsCommitDialog.uxml",
     {"vcs-commit-dialog", "commit-message-field", "commit-button", "cancel-button"}},
    {"PrefabBar",
     "UI/controls/PrefabBar/PrefabBar.uxml",
     {"prefab-bar-title", "prefab-bar-select-asset", "prefab-bar-count"}},
    {"ProjectPickerNewProjectView",
     "UI/controls/ProjectPicker/ProjectPickerNewProjectView.uxml",
     {"new-project-view", "new-project-top", "new-project-title", "new-project-back", "new-project-tabs",
      "project-templates-tab", "project-community-tab", "new-project-grid-scroll", "new-project-form",
      "new-project-name-field", "new-project-location-field", "new-project-location-browse", "new-project-error"}},
    {"ProjectPickerNewProjectGrid",
     "UI/controls/ProjectPicker/ProjectPickerNewProjectGrid.uxml",
     {"project-templates", "project-templates-empty", "project-community", "project-community-status",
      "project-community-icon", "project-community-title", "project-community-message", "project-community-retry",
      "project-community-offline", "project-community-offline-label", "project-community-refresh",
      "project-community-grid"}},
    {"ProjectPickerImportView",
     "UI/controls/ProjectPicker/ProjectPickerImportView.uxml",
     {"import-project-view", "import-header", "import-title", "import-back", "import-folder-tab", "import-archive-tab",
      "import-git-tab", "import-folder-panel", "import-folder-drop", "import-folder-browse", "import-reference-option",
      "import-archive-panel", "import-archive-drop", "import-archive-browse", "import-git-panel",
      "import-git-url-field", "import-git-hint", "import-destination-row", "import-destination-field",
      "import-destination-browse", "import-error"}},
    {"ProjectPickerNewProjectActions",
     "UI/controls/ProjectPicker/ProjectPickerNewProjectActions.uxml",
     {"new-project-actions", "new-project-cancel", "create-project-button"}},
    {"ProjectPickerImportActions",
     "UI/controls/ProjectPicker/ProjectPickerImportActions.uxml",
     {"import-project-actions", "import-cancel", "import-project-button"}},
    {"ProjectCard",
     "UI/controls/ProjectPicker/ProjectCard.uxml",
     {"project-card-preview", "project-card-preview-image", "project-card-name", "project-card-meta"}},
    {"RecentProjectNewCard", "UI/controls/ProjectPicker/RecentProjectNewCard.uxml", {}},
    {"RecentProjectOpenCard", "UI/controls/ProjectPicker/RecentProjectOpenCard.uxml", {}},
    {"ProjectPickerSearchPlaceholder", "UI/controls/ProjectPicker/ProjectPickerSearchPlaceholder.uxml", {}},
    {"ProjectPickerRecentGrid", "UI/controls/ProjectPicker/ProjectPickerRecentGrid.uxml", {"recent-projects-container"}},
}};

// Counts the error lines that hold every one of `needles`. The logger owns the sink; only the
// callback is unregistered.
class ErrorLineCounter
{
  public:
    explicit ErrorLineCounter(std::vector<std::string> needles) : m_Needles(std::move(needles))
    {
        Logger::Log::Initialize({Logger::LogLevel::Debug, false});
        auto sink = std::make_unique<Logger::CallbackSink>();
        m_Sink = sink.get();
        Logger::Log::AddSink(std::move(sink));
        m_CallbackId = m_Sink->RegisterCallback([this](const Logger::LogMessage& message) { Count(message); });
    }
    ~ErrorLineCounter() { m_Sink->UnregisterCallback(m_CallbackId); }

    int Lines() const
    {
        Logger::Log::Flush();
        return m_Count.load();
    }

  private:
    void Count(const Logger::LogMessage& message)
    {
        if (message.Level != Logger::LogLevel::Error)
            return;
        for (const std::string& needle : m_Needles)
            if (message.Message.find(needle) == std::string::npos)
                return;
        ++m_Count;
    }

    std::vector<std::string> m_Needles;
    Logger::CallbackSink* m_Sink = nullptr;
    std::uint64_t m_CallbackId = 0;
    std::atomic<int> m_Count{0};
};

std::set<std::string> IdsInLayout(const std::string& text)
{
    static const std::regex kIdAttribute(R"re(\bid="([^"]+)")re");
    std::set<std::string> ids;
    for (auto match = std::sregex_iterator(text.begin(), text.end(), kIdAttribute); match != std::sregex_iterator();
         ++match)
        ids.insert((*match)[1].str());
    return ids;
}

// The text of the shipped layout file, read as the loader reads it.
std::string ShippedText(const char* layoutAssetPath)
{
    std::string text;
    ReadFileTextShared(Editor::GetEditorGlobalPaths().installAssetsRoot / layoutAssetPath, text);
    return text;
}

class LoadControlLayoutTest : public ::testing::TestWithParam<ControlLayout>
{
};

TEST_P(LoadControlLayoutTest, TheShippedLayoutHoldsExactlyTheIdsTheControlRequires)
{
    const ControlLayout& control = GetParam();
    EXPECT_NE(Editor::ControlLayoutLoader::Load(control.LayoutAssetPath, control.RequiredIds), nullptr);

    const std::set<std::string> required(control.RequiredIds.begin(), control.RequiredIds.end());
    EXPECT_EQ(IdsInLayout(ShippedText(control.LayoutAssetPath)), required);
}

TEST_P(LoadControlLayoutTest, ALayoutWithoutOneRequiredIdLogsTheFileAndTheIdAndDoesNotLoad)
{
    const ControlLayout& control = GetParam();
    const std::string shipped = ShippedText(control.LayoutAssetPath);
    ASSERT_FALSE(shipped.empty());
    const std::filesystem::path folder =
        std::filesystem::path(::testing::TempDir()) / "LoadControlLayoutTests" / control.Name;
    std::filesystem::create_directories(folder);

    for (const std::string_view id : control.RequiredIds)
    {
        const std::string attribute = "id=\"" + std::string(id) + "\"";
        const std::filesystem::path broken = folder / (std::string(id) + ".uxml");
        std::ofstream(broken, std::ios::binary) << ReplaceAll(shipped, attribute, "id=\"renamed\"");

        // An absolute path replaces the install-asset root it is appended to.
        const std::string brokenPath = broken.generic_string();
        const ErrorLineCounter errors({brokenPath, "'" + std::string(id) + "'"});
        EXPECT_EQ(Editor::ControlLayoutLoader::Load(brokenPath.c_str(), control.RequiredIds), nullptr) << id;
        EXPECT_EQ(errors.Lines(), 1) << id;
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}

TEST(LoadControlLayoutCache, ASecondLoadOfAPathReadsNoFile)
{
    const std::filesystem::path folder = std::filesystem::path(::testing::TempDir()) / "LoadControlLayoutCache";
    std::filesystem::create_directories(folder);
    const std::filesystem::path layout = folder / "Cached.uxml";
    std::ofstream(layout, std::ios::binary) << R"(<UIElement><Label id="cached-text" text="Cached"/></UIElement>)";
    const std::string layoutPath = layout.generic_string();
    constexpr std::array<std::string_view, 1> kRequiredIds = {"cached-text"};
    ASSERT_NE(Editor::ControlLayoutLoader::Load(layoutPath.c_str(), kRequiredIds), nullptr);

    std::filesystem::remove(layout);
    const ErrorLineCounter errors({layoutPath});
    EXPECT_NE(Editor::ControlLayoutLoader::Load(layoutPath.c_str(), kRequiredIds), nullptr);
    EXPECT_EQ(errors.Lines(), 0);
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}

// The cache listens to the asset events of a started engine.
bool StartEngine()
{
    EngineCore& engine = EngineCore::GetInstance();
    if (engine.IsInitialized())
        return true;
    ApplicationConfig config{};
    config.AssetDirectory = ".";
    config.WorkspaceDirectory = ".";
    config.EnableEditor = true;
    return engine.Initialize(config);
}

TEST(LoadControlLayoutCache, AnEditedFileLoadsItsNewTextAfterItsAssetEvent)
{
    ASSERT_TRUE(StartEngine());
    EngineCore& engine = EngineCore::GetInstance();
    const std::filesystem::path folder = std::filesystem::path(::testing::TempDir()) / "LoadControlLayoutEdit";
    std::filesystem::create_directories(folder);
    const std::filesystem::path layout = folder / "Edited.uxml";
    const std::string layoutPath = layout.generic_string();
    constexpr std::array<std::string_view, 1> kFirstIds = {"first-text"};
    std::ofstream(layout, std::ios::binary) << R"(<UIElement><Label id="first-text" text="First"/></UIElement>)";
    ASSERT_NE(Editor::ControlLayoutLoader::Load(layoutPath.c_str(), kFirstIds), nullptr);

    // A save in place sends Modified; a save by delete and create sends Created; a delete sends Destroyed.
    constexpr std::array<AssetEventType, 3> kEdits = {AssetEventType::AssetModified, AssetEventType::AssetCreated,
                                                      AssetEventType::AssetDestroyed};
    for (const AssetEventType edit : kEdits)
    {
        const std::string id = "text-" + std::to_string(static_cast<int>(edit));
        const std::array<std::string_view, 1> editIds = {id};
        std::ofstream(layout, std::ios::binary) << "<UIElement><Label id=\"" << id << "\" text=\"Edit\"/></UIElement>";
        engine.GetAssetManager().GetEventDispatcher().DispatchEvent(
            AssetEvent(edit, GUID::Null(), AssetType::UILayout, layout.string()));
        EXPECT_NE(Editor::ControlLayoutLoader::Load(layoutPath.c_str(), editIds), nullptr) << static_cast<int>(edit);
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}

TEST(LoadControlLayoutCache, AnEditAfterTheDispatcherDropsItsCallbacksLoadsTheNewText)
{
    ASSERT_TRUE(StartEngine());
    AssetEventDispatcher& dispatcher = EngineCore::GetInstance().GetAssetManager().GetEventDispatcher();
    const std::filesystem::path folder = std::filesystem::path(::testing::TempDir()) / "LoadControlLayoutRelisten";
    std::filesystem::create_directories(folder);
    const std::filesystem::path layout = folder / "Relisten.uxml";
    const std::string layoutPath = layout.generic_string();
    constexpr std::array<std::string_view, 1> kFirstIds = {"first-text"};
    constexpr std::array<std::string_view, 1> kEditIds = {"edit-text"};
    std::ofstream(layout, std::ios::binary) << R"(<UIElement><Label id="first-text" text="First"/></UIElement>)";
    ASSERT_NE(Editor::ControlLayoutLoader::Load(layoutPath.c_str(), kFirstIds), nullptr);

    // The same dispatcher without the callback, as when a restarted engine puts its dispatcher at the old address.
    dispatcher.Clear();
    ASSERT_NE(Editor::ControlLayoutLoader::Load(layoutPath.c_str(), kFirstIds), nullptr);
    std::ofstream(layout, std::ios::binary) << R"(<UIElement><Label id="edit-text" text="Edit"/></UIElement>)";
    dispatcher.DispatchEvent(AssetEvent(AssetEventType::AssetModified, GUID::Null(), AssetType::UILayout, layout.string()));
    EXPECT_NE(Editor::ControlLayoutLoader::Load(layoutPath.c_str(), kEditIds), nullptr);
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}

TEST(LoadControlLayoutCache, AnEditPendingWhenTheDispatcherDropsItsCallbacksLoadsTheNewText)
{
    ASSERT_TRUE(StartEngine());
    AssetEventDispatcher& dispatcher = EngineCore::GetInstance().GetAssetManager().GetEventDispatcher();
    const std::filesystem::path folder = std::filesystem::path(::testing::TempDir()) / "LoadControlLayoutPending";
    std::filesystem::create_directories(folder);
    const std::filesystem::path layout = folder / "Pending.uxml";
    const std::string layoutPath = layout.generic_string();
    constexpr std::array<std::string_view, 1> kFirstIds = {"first-text"};
    constexpr std::array<std::string_view, 1> kEditIds = {"edit-text"};
    std::ofstream(layout, std::ios::binary) << R"(<UIElement><Label id="first-text" text="First"/></UIElement>)";
    ASSERT_NE(Editor::ControlLayoutLoader::Load(layoutPath.c_str(), kFirstIds), nullptr);

    std::ofstream(layout, std::ios::binary) << R"(<UIElement><Label id="edit-text" text="Edit"/></UIElement>)";
    dispatcher.DispatchEvent(AssetEvent(AssetEventType::AssetModified, GUID::Null(), AssetType::UILayout, layout.string()));
    dispatcher.Clear();
    EXPECT_NE(Editor::ControlLayoutLoader::Load(layoutPath.c_str(), kEditIds), nullptr);
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}

std::string ControlName(const ::testing::TestParamInfo<ControlLayout>& info)
{
    return info.param.Name;
}

INSTANTIATE_TEST_SUITE_P(EditorControls, LoadControlLayoutTest, ::testing::ValuesIn(kControlLayouts), ControlName);

} // namespace
} // namespace GameEngine
