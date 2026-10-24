#include "Docking/WorkingLayoutPersistence.h"

#include "Editor/Registries/EditorPanelRegistry.h"
#include "Editor/Settings/SettingsStore.h"
#include "EditorDockNodeJson.h"
#include "Logger/Logger.h"
#include "Types/StringUtils.h"
#include "UI/Layout/DockLayoutController.h"
#include "UI/Layout/Docking.h"

#include <algorithm>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace GameEngine::Editor
{
namespace
{
constexpr const char* kWorkingLayoutKey = "ui.workingLayout";
constexpr const char* kLayoutField = "layout";
constexpr const char* kPackagePanelsField = "packagePanels";
constexpr const char* kSeparateWindowPanelsField = "separateWindowPanels";

void AppendPanelIds(const DockNode* node, std::vector<std::string>& out)
{
    if (!node)
        return;
    if (node->IsLeaf())
    {
        out.insert(out.end(), node->GetTabs().begin(), node->GetTabs().end());
        return;
    }
    AppendPanelIds(node->First(), out);
    AppendPanelIds(node->Second(), out);
}

std::vector<std::string> ReadStringArray(const nlohmann::json& record, const char* field)
{
    std::vector<std::string> out;
    const auto it = record.find(field);
    if (it == record.end() || !it->is_array())
        return out;
    for (const auto& entry : *it)
    {
        if (entry.is_string())
            out.push_back(entry.get<std::string>());
    }
    return out;
}

bool Contains(const std::vector<std::string>& ids, const std::string& id)
{
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}
} // namespace

void WorkingLayoutPersistence::Save(const std::filesystem::path& workspaceRoot,
                                    const DockNode* layout,
                                    DockingManager& docking,
                                    std::span<const std::string> separateWindowPanelIds)
{
    if (workspaceRoot.empty() || !layout)
        return;

    std::vector<std::string> panelIds;
    AppendPanelIds(layout, panelIds);
    std::vector<std::string> packagePanels;
    EditorPanelDescriptor descriptor;
    for (const std::string& id : panelIds)
    {
        if (EditorPanelRegistry::Get().TryGetPanel(id, descriptor))
            packagePanels.push_back(id);
    }
    // A placeholder whose package never registered this session is dropped
    // here; kept, the next Load would read it as an unknown built-in panel.
    std::unique_ptr<DockNode> saved = DockLayoutController(docking).CloneForRegisteredPanels(layout, packagePanels);
    if (!saved)
        return;

    nlohmann::json record = nlohmann::json::object();
    record[kLayoutField] = DockNodeToJson(saved.get());
    record[kPackagePanelsField] = packagePanels;
    record[kSeparateWindowPanelsField] = nlohmann::json(
        std::vector<std::string>(separateWindowPanelIds.begin(), separateWindowPanelIds.end()));

    SettingsStore store = OpenUserProjectSettings(workspaceRoot);
    std::string error;
    (void)store.Load(&error); // a missing file is created by Save
    store.SetJson(kWorkingLayoutKey, record);
    if (!store.Save(&error))
    {
        Logger::Log::Warning("Editor: could not save the panel layout to '{}': {}",
                             store.GetFilePath().string(), error);
    }
}

std::unique_ptr<DockNode> WorkingLayoutPersistence::Load(const std::filesystem::path& workspaceRoot,
                                                         DockingManager& docking)
{
    if (workspaceRoot.empty())
        return nullptr;

    SettingsStore store = OpenUserProjectSettings(workspaceRoot);
    std::string error;
    if (!store.Load(&error))
    {
        Logger::Log::Warning("Editor: could not read the saved panel layout in '{}' ({}); using the active layout preset",
                             store.GetFilePath().string(), error);
        return nullptr;
    }

    const auto itRecord = store.Json().find(kWorkingLayoutKey);
    if (itRecord == store.Json().end())
        return nullptr;

    std::unique_ptr<DockNode> saved;
    if (itRecord->is_object())
    {
        const auto itLayout = itRecord->find(kLayoutField);
        if (itLayout != itRecord->end())
            saved = DockNodeFromJson(*itLayout);
    }
    if (!saved)
    {
        Logger::Log::Warning("Editor: the saved panel layout in '{}' is not a valid layout; using the active layout preset",
                             store.GetFilePath().string());
        return nullptr;
    }

    std::vector<std::string> panelIds;
    AppendPanelIds(saved.get(), panelIds);
    std::vector<std::string> sortedIds = panelIds;
    std::sort(sortedIds.begin(), sortedIds.end());
    if (std::adjacent_find(sortedIds.begin(), sortedIds.end()) != sortedIds.end())
    {
        Logger::Log::Warning("Editor: the saved panel layout in '{}' is not a valid layout; using the active layout preset",
                             store.GetFilePath().string());
        return nullptr;
    }

    const std::vector<std::string> packagePanels = ReadStringArray(*itRecord, kPackagePanelsField);
    std::vector<std::string> unknownPanels;
    std::vector<std::string> pendingPackagePanels;
    for (const std::string& id : panelIds)
    {
        if (docking.GetPanel(id))
            continue;
        if (Contains(packagePanels, id))
            pendingPackagePanels.push_back(id);
        else
            unknownPanels.push_back(id);
    }
    if (!unknownPanels.empty())
    {
        Logger::Log::Warning("Editor: the saved panel layout names panels this editor does not have ({}); using the active layout preset",
                             Join(unknownPanels, ", "));
        return nullptr;
    }

    std::unique_ptr<DockNode> restored =
        DockLayoutController(docking).CloneForRegisteredPanels(saved.get(), pendingPackagePanels);
    if (!restored)
    {
        Logger::Log::Warning("Editor: the saved panel layout holds no panel this editor has; using the active layout preset");
        return nullptr;
    }

    if (!pendingPackagePanels.empty())
    {
        Logger::Log::Info("Editor: the saved panel layout keeps the place of package panels that register later ({})",
                          Join(pendingPackagePanels, ", "));
    }
    const std::vector<std::string> separateWindowPanels = ReadStringArray(*itRecord, kSeparateWindowPanelsField);
    if (!separateWindowPanels.empty())
    {
        Logger::Log::Info("Editor: panels that were in separate windows start closed ({})",
                          Join(separateWindowPanels, ", "));
    }
    Logger::Log::Info("Editor: restored the saved panel layout from '{}'", store.GetFilePath().string());
    return restored;
}

} // namespace GameEngine::Editor
