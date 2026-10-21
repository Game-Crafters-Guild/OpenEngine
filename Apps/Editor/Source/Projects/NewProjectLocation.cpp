#include "Projects/NewProjectLocation.h"

#include "Editor/Settings/SettingsStore.h"
#include "Logger/Logger.h"

#include <string>
#include <system_error>

namespace GameEngine::Editor
{

std::filesystem::path LoadNewProjectLocation(const std::filesystem::path& fallback)
{
    SettingsStore prefs = OpenEditorPreferences();
    std::string err;
    (void)prefs.Load(&err);
    std::string location;
    if (!prefs.TryGetString(kNewProjectLocationPrefKey, location) || location.empty())
        return fallback;
    const std::filesystem::path folder(location);
    std::error_code ec;
    return std::filesystem::is_directory(folder, ec) ? folder : fallback;
}

void SaveNewProjectLocation(const std::filesystem::path& parent)
{
    SettingsStore prefs = OpenEditorPreferences();
    std::string err;
    (void)prefs.Load(&err);
    prefs.SetString(kNewProjectLocationPrefKey, parent.generic_string());
    if (!prefs.Save(&err))
        Logger::Log::Warning("Project picker: could not remember the new project location: {}", err);
}

} // namespace GameEngine::Editor
