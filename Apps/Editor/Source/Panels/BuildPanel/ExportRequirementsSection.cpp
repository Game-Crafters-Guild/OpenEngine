#include "Panels/BuildPanel/ExportRequirementsSection.h"

#include "Core/Engine.h"
#include "Engine/Build/CancellableShellProcess.h"
#include "Engine/Build/ExportRequirements.h"
#include "JobSystem/JobChannel.h"
#include "Logger/Logger.h"
#include "Panels/WebPanel.h"
#include "Platform/Shell.h"
#include "UI/ControlLayoutLoader.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/InspectorNotice.h"
#include "UI/StyleUtil.h"
#include "UI/UIEvents.h"
#include "UI/UiPostHandle.h"

#include <algorithm>
#include <array>
#include <format>

namespace GameEngine
{
namespace
{
constexpr const char* kLayoutAssetPath = "UI/controls/BuildPanel/ExportRequirementsSection.uxml";
constexpr const char* kStyleAssetPath = "UI/controls/BuildPanel/ExportRequirementsSection.css";
constexpr std::string_view kRowsId = "ExportRequirementRows";
constexpr std::string_view kDocsButtonId = "ExportRequirementsDocs";
// The "Export requirements" section of docs/publishing/game-publishing.mdx.
constexpr std::string_view kDocsPage = "publishing/game-publishing/#export-requirements";

using EditorUI::InspectorNotice;

int RunShellCommand(const std::string& command, const std::function<void(const std::string& line)>& onLine,
                    const std::function<bool()>& shouldStop)
{
    CancellableShellProcess process;
    return process.Run(command + " 2>&1", shouldStop, onLine).exitCode;
}
} // namespace

ExportRequirementsSection::ExportRequirementsSection()
    : ExportRequirementsSection(ExportRequirementWork{})
{
    m_Work.RunCommand = RunShellCommand;
    m_Work.Start = [this](std::function<void()> work) { SubmitJob(std::move(work)); };
    m_Work.Post = [post = GetPostHandle()](std::function<void()> action) { post.Post(std::move(action)); };
}

ExportRequirementsSection::ExportRequirementsSection(ExportRequirementWork work)
    : m_Work(std::move(work))
{
    AddClass("export-requirements");
    RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");
    constexpr std::array<std::string_view, 2> requiredIds{kRowsId, kDocsButtonId};
    std::unique_ptr<UIElement> layout = Editor::ControlLayoutLoader::Load(kLayoutAssetPath, requiredIds);
    if (!layout)
        return;
    m_RowsContainer = layout->FindById<UIElement>(HashStringId(kRowsId)).Get();
    if (Button* docs = layout->FindById<Button>(HashStringId(kDocsButtonId)).Get())
    {
        docs->SetTooltip("Open the export requirements page of the documentation.");
        docs->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OpenDocs(); });
    }
    AddChild(std::move(layout));
    UIUtil::SetShown(*this, false);
}

ExportRequirementsSection::~ExportRequirementsSection()
{
    // Stop, then wait: a running command polls m_Stopping, and a queued job returns at once.
    m_Stopping.store(true);
    for (JobSystem::TaskHandle& job : m_Jobs)
        if (job.IsValid())
            job.Wait();
}

void ExportRequirementsSection::ShowRequirements(std::vector<const ExportRequirement*> requirements)
{
    std::vector<Row> rows;
    std::vector<const ExportRequirement*> added;
    for (const ExportRequirement* requirement : requirements)
    {
        if (Row* existing = FindRow(requirement))
        {
            rows.push_back(std::move(*existing));
            continue;
        }
        rows.push_back(Row{.Requirement = requirement});
        added.push_back(requirement);
    }
    // A moved row has no StopInstall. A row that goes stops its install: no row can show or cancel it.
    for (const Row& dropped : m_Rows)
        if (dropped.StopInstall)
            dropped.StopInstall->store(true);
    m_Rows = std::move(rows);
    UIUtil::SetShown(*this, !m_Rows.empty());
    RebuildRows();
    if (!added.empty())
        StartChecks(std::move(added));
}

void ExportRequirementsSection::Recheck()
{
    std::vector<const ExportRequirement*> requirements;
    for (Row& row : m_Rows)
    {
        if (row.State == RowState::Installing)
            continue;
        row.State = RowState::Checking;
        requirements.push_back(row.Requirement);
    }
    RebuildRows();
    if (!requirements.empty())
        StartChecks(std::move(requirements));
}

void ExportRequirementsSection::SetOnOpenDocsPage(std::function<void(std::string_view page)> onOpenDocsPage)
{
    m_OnOpenDocsPage = std::move(onOpenDocsPage);
}

void ExportRequirementsSection::OnOwnerManagerChanged(UIManager* owner)
{
    if (!owner)
        return;
    std::vector<const ExportRequirement*> checking;
    for (const Row& row : m_Rows)
        if (row.State == RowState::Checking)
            checking.push_back(row.Requirement);
    if (!checking.empty())
        StartChecks(std::move(checking));
}

ExportRequirementsSection::Row* ExportRequirementsSection::FindRow(const ExportRequirement* requirement)
{
    auto it = std::ranges::find(m_Rows, requirement, &Row::Requirement);
    return it == m_Rows.end() ? nullptr : &*it;
}

void ExportRequirementsSection::StartChecks(std::vector<const ExportRequirement*> requirements)
{
    m_Work.Start([this, requirements = std::move(requirements)]() { RunChecks(requirements); });
}

void ExportRequirementsSection::RunChecks(const std::vector<const ExportRequirement*>& requirements)
{
    const auto shouldStop = [this]() { return m_Stopping.load(); };
    const auto runCheck = [&](const std::string& command) { return m_Work.RunCommand(command, {}, shouldStop); };
    for (const ExportRequirement* requirement : requirements)
    {
        if (m_Stopping.load())
            return;
        const bool installed = IsExportRequirementInstalled(*requirement, runCheck);
        m_Work.Post([this, requirement, installed]() { ApplyCheck(requirement, installed); });
    }
}

void ExportRequirementsSection::ApplyCheck(const ExportRequirement* requirement, bool installed)
{
    Row* row = FindRow(requirement);
    if (!row || row->State != RowState::Checking)
        return;
    if (installed)
        row->State = RowState::Installed;
    else
        row->State = requirement->Fallback.empty() ? RowState::Missing : RowState::Optional;
    RebuildRows();
}

void ExportRequirementsSection::Install(const ExportRequirement* requirement)
{
    Row* row = FindRow(requirement);
    if (!row || row->State == RowState::Installing)
        return;
    row->State = RowState::Installing;
    row->Detail = std::format("Running '{}'.", requirement->InstallCommand);
    row->StopInstall = std::make_shared<std::atomic<bool>>(false);
    RebuildRows();
    m_Work.Start([this, requirement, stop = row->StopInstall]() { RunInstall(requirement, stop); });
}

void ExportRequirementsSection::RunInstall(const ExportRequirement* requirement, std::shared_ptr<std::atomic<bool>> stop)
{
    std::string firstLine;
    const auto onLine = [&](const std::string& line) {
        if (line.empty())
            return;
        if (firstLine.empty())
            firstLine = line;
        m_Work.Post([this, requirement, line]() { ShowInstallOutput(requirement, line); });
    };
    const auto shouldStop = [this, &stop]() { return m_Stopping.load() || stop->load(); };
    const std::string command(requirement->InstallCommand);
    const int exitCode = m_Work.RunCommand(command, onLine, shouldStop);
    const bool cancelled = stop->load();
    if (cancelled)
        Logger::Log::Info("Build: '{}' was cancelled", command);
    else if (exitCode == 0)
        Logger::Log::Info("Build: '{}' finished", command);
    else
        Logger::Log::Warning("Build: '{}' exited with code {}: {}", command, exitCode, firstLine);
    m_Work.Post([this, requirement, exitCode, cancelled, firstLine]() {
        FinishInstall(requirement, exitCode, cancelled, firstLine);
    });
}

void ExportRequirementsSection::ShowInstallOutput(const ExportRequirement* requirement, const std::string& line)
{
    Row* row = FindRow(requirement);
    if (!row || row->State != RowState::Installing)
        return;
    row->Detail = line;
    // In place: a new notice would replace the Cancel button between a press and its release.
    if (row->Notice)
        row->Notice->SetText(line);
    else
        RebuildRows();
}

void ExportRequirementsSection::FinishInstall(const ExportRequirement* requirement, int exitCode, bool cancelled,
                                              const std::string& firstLine)
{
    Row* row = FindRow(requirement);
    if (!row)
        return;
    row->StopInstall.reset();
    if (exitCode == 0 || cancelled)
    {
        row->State = RowState::Checking;
        RebuildRows();
        StartChecks({requirement});
        return;
    }
    row->State = RowState::InstallFailed;
    row->Detail = std::format("'{}' exited with code {}{}{}", requirement->InstallCommand, exitCode,
                              firstLine.empty() ? "" : ": ", firstLine.empty() ? "." : firstLine);
    RebuildRows();
}

void ExportRequirementsSection::CancelInstall(const ExportRequirement* requirement)
{
    if (Row* row = FindRow(requirement); row && row->StopInstall)
        row->StopInstall->store(true);
}

void ExportRequirementsSection::OpenDocs()
{
    if (m_OnOpenDocsPage)
        m_OnOpenDocsPage(kDocsPage);
    else
        (void)Platform::OpenUrl(std::string(WebPanel::kHelpDocsURL) + std::string(kDocsPage));
}

void ExportRequirementsSection::SubmitJob(std::function<void()> work)
{
    if (!m_Channel)
        m_Channel = std::make_unique<JobSystem::JobChannel>(
            EngineCore::GetInstance().GetJobSystem(),
            JobSystem::JobChannelDesc{.Name = "Export requirements", .MaxRunning = 1});
    std::erase_if(m_Jobs, [](const JobSystem::TaskHandle& job) { return !job.IsValid() || job.IsDone(); });
    m_Jobs.push_back(m_Channel->Submit(std::move(work)));
}

void ExportRequirementsSection::RebuildRows()
{
    // A row's own action calls this: its notice must outlive the click that runs it.
    if (UIElement::IsInEventDispatch())
    {
        if (!m_RebuildRowsPosted)
            m_RebuildRowsPosted = PostAction([this]() {
                m_RebuildRowsPosted = false;
                RebuildRows();
            });
        return;
    }
    if (!m_RowsContainer)
        return;
    for (Row& row : m_Rows)
        row.Notice = nullptr;
    m_RowsContainer->RemoveAllChildren();
    for (Row& row : m_Rows)
        AddRowNotice(row);
}

void ExportRequirementsSection::AddRowNotice(Row& row)
{
    const ExportRequirement* requirement = row.Requirement;
    auto kind = InspectorNotice::Kind::Information;
    auto glyph = InspectorNotice::Glyph::OfKind;
    std::string status;
    std::string text;
    std::string actionLabel = "Install";
    std::string actionTooltip = std::format("Run '{}' in the background.", requirement->InstallCommand);
    std::function<void()> action = [this, requirement]() { Install(requirement); };
    switch (row.State)
    {
    case RowState::Checking:
        status = "checking";
        glyph = InspectorNotice::Glyph::Clock;
        action = nullptr;
        break;
    case RowState::Installed:
        status = "installed";
        glyph = InspectorNotice::Glyph::Check;
        action = nullptr;
        break;
    case RowState::Missing:
        kind = InspectorNotice::Kind::Error;
        status = "not installed";
        text = "The export needs it.";
        break;
    case RowState::Optional:
        kind = InspectorNotice::Kind::Warning;
        status = "not installed (optional)";
        text = requirement->Fallback;
        break;
    case RowState::Installing:
        glyph = InspectorNotice::Glyph::Clock;
        status = "installing";
        text = row.Detail;
        actionLabel = "Cancel";
        actionTooltip = "Stop the install.";
        action = [this, requirement]() { CancelInstall(requirement); };
        break;
    case RowState::InstallFailed:
        kind = InspectorNotice::Kind::Error;
        status = "install failed";
        text = row.Detail + " Install tries again. Docs shows how to install it yourself.";
        break;
    }
    auto notice = std::make_unique<InspectorNotice>(text, kind);
    notice->SetTitle(std::format("{}: {}", requirement->Name, status));
    notice->SetGlyph(glyph);
    if (action)
        notice->SetAction(actionLabel, actionTooltip, std::move(action));
    row.Notice = notice.get();
    m_RowsContainer->AddChild(std::move(notice));
}

} // namespace GameEngine
