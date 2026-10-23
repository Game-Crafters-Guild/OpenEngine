#pragma once

#include "UI/UIElement.h"

#include <filesystem>
#include <functional>
#include <string>

namespace GameEngine
{
class Label;
class TextField;
namespace ScriptsProjectFile
{
struct PackageReference;
}
} // namespace GameEngine

namespace GameEngine::Editor
{

// The Script Packages page's list: one row per PackageReference of the scripts csproj, a row that
// adds a package, and a status line. Every edit is written into the csproj at once, and under
// central package versions the version into Directory.Packages.props; the script manager watches
// both files, so a saved change restores the packages and compiles the scripts.
// The layout is ScriptPackagesList.uxml and ScriptPackageRow.uxml, the style ScriptPackagesList.css
// (UI/controls/ScriptPackagesList/ under the editor asset mount).
class ScriptPackagesList : public UIElement
{
  public:
    explicit ScriptPackagesList(std::filesystem::path projectPath);

    // Adds the package named in the add row's two fields, or says on the status line why it cannot.
    void AddPackage();

    // Removes the package from the csproj; its row goes only when the file was written. A central
    // PackageVersion stays in Directory.Packages.props, which other projects may share.
    void RemovePackage(const std::string& id);

  private:
    void AppendRow(const ScriptsProjectFile::PackageReference& package);
    // Writes a valid version into the csproj; an invalid one is refused, and `field` shows the
    // version the file holds again.
    void ChangeVersion(TextField& field, const std::string& id, const std::string& version);
    // False, with the fix on the status line, when the project manages versions centrally and has
    // no Directory.Packages.props to write them into.
    bool CanWriteVersions();
    // Applies `edit` to the text of `file` and writes it back; false, with the reason on the status
    // line, when the file cannot be read or written.
    bool EditFile(const std::filesystem::path& file, const std::function<void(std::string&)>& edit);
    UIElement* FindRow(const std::string& id) const;

    std::filesystem::path m_ProjectPath;
    // Under central package versions (ManagePackageVersionsCentrally), the versions are the
    // PackageVersion items of m_VersionsFile and the csproj's PackageReference items carry none.
    bool m_CentralVersions = false;
    std::filesystem::path m_VersionsFile;
    UIElement* m_Rows = nullptr;
    TextField* m_NewId = nullptr;
    TextField* m_NewVersion = nullptr;
    Label* m_Status = nullptr;
};

} // namespace GameEngine::Editor
