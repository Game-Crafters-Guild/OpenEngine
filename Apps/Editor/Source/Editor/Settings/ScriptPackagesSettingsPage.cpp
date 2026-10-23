#include "Editor/Settings/ScriptPackagesSettingsPage.h"

#include "Core/Engine.h"
#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/ScriptPackagesList.h"
#include "Scripting/ScriptManager.h"

#include <memory>
#include <utility>

namespace GameEngine::Editor
{

void RegisterScriptPackagesSettingsCategory()
{
    SettingsCategoryDescriptor page;
    page.CategoryId = "scriptPackages";
    page.Title = "Script Packages";
    page.Group = SettingsCategoryGroup::ProjectSettings;
    page.SearchKeywords = "script packages nuget dependencies csproj packagereference library c#";
    page.Description =
        "The NuGet packages the project's C# scripts use. This list is the PackageReference items of "
        "GameEngine.Scripts.csproj at the project root: a package added in the IDE shows here, and one added here "
        "shows in the IDE. The editor restores the packages and compiles the scripts against them; an exported "
        "game ships them. Change a version in its field, or remove a package with its button.";

    SettingsFieldDescriptor list;
    list.Label = "Packages";
    list.SearchKeywords = "nuget package id version add remove";
    SettingsFieldDescriptor::CustomField control;
    control.CreateRow = []()
    { return std::make_unique<ScriptPackagesList>(EngineCore::GetInstance().GetScriptManager().GeneratedProjectPath()); };
    list.Control = std::move(control);
    page.Fields.push_back(std::move(list));

    EditorSettingsRegistry::Get().RegisterCategory(std::move(page));
}

} // namespace GameEngine::Editor
