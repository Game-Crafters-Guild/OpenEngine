#include "Editor/Settings/StartupSettingsPage.h"

#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/SettingsStore.h"

#include <utility>

namespace GameEngine::Editor
{
namespace
{

constexpr const char* kAutoLoadLastProjectPrefKey = "startup.autoLoadLastProject";
constexpr bool kAutoLoadLastProjectByDefault = false;

} // namespace

bool IsAutoLoadLastProjectOn(const SettingsStore& preferences)
{
    bool autoLoad = kAutoLoadLastProjectByDefault;
    preferences.TryGetBool(kAutoLoadLastProjectPrefKey, autoLoad);
    return autoLoad;
}

void RegisterStartupSettingsCategory()
{
    SettingsCategoryDescriptor startup;
    startup.CategoryId = "startup";
    startup.Title = "Startup";
    startup.Group = SettingsCategoryGroup::UserSettings;
    startup.SearchKeywords = "startup start launch open project picker";
    startup.Description = "Startup sets what the editor opens when it starts without a project.";

    SettingsFieldDescriptor autoLoad;
    autoLoad.Label = "Auto-load last project";
    autoLoad.Tooltip =
        "Open the project you had open last time instead of the project picker. Off by default.";
    autoLoad.SearchKeywords = "auto load last project picker startup reopen";
    autoLoad.PrefKey = kAutoLoadLastProjectPrefKey;
    SettingsFieldDescriptor::ToggleField toggle;
    toggle.DefaultValue = kAutoLoadLastProjectByDefault;
    autoLoad.Control = std::move(toggle);
    startup.Fields.push_back(std::move(autoLoad));

    EditorSettingsRegistry::Get().RegisterCategory(std::move(startup));
}

} // namespace GameEngine::Editor
