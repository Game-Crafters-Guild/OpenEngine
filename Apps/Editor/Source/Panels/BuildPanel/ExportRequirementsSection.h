#pragma once

#include "JobSystem/TaskHandle.h"
#include "UI/UIElement.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace JobSystem
{
class JobChannel;
}

namespace GameEngine
{
struct ExportRequirement;

namespace EditorUI
{
class InspectorNotice;
}

/// The work behind the Requirements section. The editor runs shell commands on the job system and
/// posts to the UI thread; a test supplies its own.
struct ExportRequirementWork
{
    /// Runs @p command off the UI thread and gives its exit code. Each output line goes to @p onLine.
    /// The command stops when @p shouldStop returns true.
    std::function<int(const std::string& command, const std::function<void(const std::string& line)>& onLine,
                      const std::function<bool()>& shouldStop)>
        RunCommand;
    /// Runs @p work off the UI thread.
    std::function<void(std::function<void()> work)> Start;
    /// Runs @p action on the UI thread if the section still exists.
    std::function<void(std::function<void()> action)> Post;
};

/// The Build panel's Requirements section: one row for each tool that an export of the enabled platforms
/// needs. Install runs the tool's install command in the background, then checks the tool again.
/// The layout is UI/controls/BuildPanel/ExportRequirementsSection.uxml.
class ExportRequirementsSection final : public UIElement
{
  public:
    ExportRequirementsSection();
    explicit ExportRequirementsSection(ExportRequirementWork work);
    ~ExportRequirementsSection() override;

    /// Shows one row for each of @p requirements and checks the new rows. A row that stays keeps its state.
    /// A row that goes stops its install.
    void ShowRequirements(std::vector<const ExportRequirement*> requirements);

    /// Checks each row again, except a row that installs now.
    void Recheck();

    /// Sets what the Docs button calls with the page path under WebPanel::kHelpDocsURL. Without it, the
    /// button opens the page in the system browser.
    void SetOnOpenDocsPage(std::function<void(std::string_view page)> onOpenDocsPage);

  private:
    enum class RowState
    {
        Checking,
        Installed,
        // Not installed, and the export cannot do without it.
        Missing,
        // Not installed, and the export does the requirement's fallback.
        Optional,
        Installing,
        InstallFailed,
    };

    struct Row
    {
        const ExportRequirement* Requirement = nullptr;
        RowState State = RowState::Checking;
        // The latest install output line, or why the install failed.
        std::string Detail;
        // Set by Cancel; the install job polls it.
        std::shared_ptr<std::atomic<bool>> StopInstall;
        // The row's notice in the rows container. Null until RebuildRows adds it.
        EditorUI::InspectorNotice* Notice = nullptr;
    };

    // A check that ends before the section has a manager posts its result nowhere: checks those rows again.
    void OnOwnerManagerChanged(UIManager* owner) override;

    Row* FindRow(const ExportRequirement* requirement);
    void StartChecks(std::vector<const ExportRequirement*> requirements);
    void RunChecks(const std::vector<const ExportRequirement*>& requirements);
    void ApplyCheck(const ExportRequirement* requirement, bool installed);
    void Install(const ExportRequirement* requirement);
    void RunInstall(const ExportRequirement* requirement, std::shared_ptr<std::atomic<bool>> stop);
    void ShowInstallOutput(const ExportRequirement* requirement, const std::string& line);
    void FinishInstall(const ExportRequirement* requirement, int exitCode, bool cancelled, const std::string& firstLine);
    void CancelInstall(const ExportRequirement* requirement);
    void OpenDocs();
    void SubmitJob(std::function<void()> work);
    void RebuildRows();
    void AddRowNotice(Row& row);

    ExportRequirementWork m_Work;
    std::function<void(std::string_view page)> m_OnOpenDocsPage;
    std::vector<Row> m_Rows;
    UIElement* m_RowsContainer = nullptr;
    bool m_RebuildRowsPosted = false;
    // Set by the destructor: every command stops and no job starts another.
    std::atomic<bool> m_Stopping{false};
    // "Export requirements" (cap 1): checks and installs wait on other processes. Created by the first job.
    std::unique_ptr<JobSystem::JobChannel> m_Channel;
    std::vector<JobSystem::TaskHandle> m_Jobs;
};

} // namespace GameEngine
