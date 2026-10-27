#include "Editor/Settings/ScriptPackagesList.h"

#include "AssetCore/SharedFileRead.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Logger/Logger.h"
#include "Scripting/ScriptPackageRestore.h"
#include "Scripting/ScriptsProjectFile.h"
#include "Types/StringUtils.h"
#include "UI/ControlLayoutLoader.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextField.h"
#include "UI/UIEvents.h"

#include <memory>
#include <utility>
#include <vector>

namespace GameEngine::Editor
{
namespace
{
constexpr const char* kListLayoutAssetPath = "UI/controls/ScriptPackagesList/ScriptPackagesList.uxml";
constexpr const char* kRowLayoutAssetPath = "UI/controls/ScriptPackagesList/ScriptPackageRow.uxml";
constexpr const char* kStyleAssetPath = "UI/controls/ScriptPackagesList/ScriptPackagesList.css";
constexpr const char* kRowsClass = "script-packages-rows";
constexpr const char* kRowClass = "script-packages-row";
constexpr const char* kIdClass = "script-packages-id";
constexpr const char* kVersionClass = "script-packages-version";
constexpr const char* kNewIdClass = "script-packages-new-id";
constexpr const char* kNewVersionClass = "script-packages-new-version";
constexpr const char* kAddClass = "script-packages-add";
constexpr const char* kRemoveClass = "script-packages-remove";
constexpr const char* kStatusClass = "script-packages-status";

// The children of `parent` that carry `className`, in order.
std::vector<UIElement*> ChildrenWithClass(UIElement* parent, const char* className)
{
    std::vector<UIElement*> found;
    for (const auto& child : parent->GetChildren())
    {
        if (child->HasClass(className))
            found.push_back(child.get());
    }
    return found;
}

// The version `file` declares for the package `id`: its PackageVersion item under central package
// versions, else its PackageReference item. Empty when the file has none or cannot be read.
std::string SavedVersion(const std::filesystem::path& file, bool central, const std::string& id)
{
    String text;
    if (file.empty() || !ReadFileTextShared(file, text))
        return {};
    if (central)
    {
        for (const ScriptsProjectFile::PackageVersion& package : ScriptsProjectFile::ReadPackageVersions(text))
        {
            if (EqualsIgnoreCase(package.Id, id))
                return package.Version;
        }
        return {};
    }
    for (const ScriptsProjectFile::PackageReference& package : ScriptsProjectFile::ReadPackageReferences(text))
    {
        if (EqualsIgnoreCase(package.Id, id))
            return package.Version;
    }
    return {};
}
} // namespace

ScriptPackagesList::ScriptPackagesList(std::filesystem::path projectPath) : m_ProjectPath(std::move(projectPath))
{
    RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");
    std::unique_ptr<UIElement> root = Editor::ControlLayoutLoader::Load(kListLayoutAssetPath, {});
    if (!root)
        return;
    UIElement* rootRaw = root.get();
    AddChild(std::move(root));

    m_Rows = InspectorDrag::FindChildByClass(rootRaw, kRowsClass);
    m_Status = dynamic_cast<Label*>(InspectorDrag::FindChildByClass(rootRaw, kStatusClass));
    const std::vector<UIElement*> rows = ChildrenWithClass(rootRaw, kRowClass);
    UIElement* addRow = rows.empty() ? nullptr : rows.front();
    m_NewId = dynamic_cast<TextField*>(InspectorDrag::FindChildByClass(addRow, kNewIdClass));
    m_NewVersion = dynamic_cast<TextField*>(InspectorDrag::FindChildByClass(addRow, kNewVersionClass));
    auto* add = dynamic_cast<Button*>(InspectorDrag::FindChildByClass(addRow, kAddClass));
    if (!m_Rows || !m_Status || !m_NewId || !m_NewVersion || !add)
    {
        Logger::Log::Error("Script Packages: {} needs the '{}' list, the '{}' and '{}' fields, the '{}' button and "
                           "the '{}' label.",
                           kListLayoutAssetPath, kRowsClass, kNewIdClass, kNewVersionClass, kAddClass, kStatusClass);
        m_Rows = nullptr;
        return;
    }
    add->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { AddPackage(); });

    String text;
    if (!ReadFileTextShared(m_ProjectPath, text))
    {
        m_Status->SetText("This project has no scripts project yet: it is created when the project opens.");
        return;
    }
    const CentralPackageVersions central = FindCentralPackageVersions(m_ProjectPath);
    m_CentralVersions = central.Enabled;
    m_VersionsFile = central.File;
    for (ScriptsProjectFile::PackageReference package : ScriptsProjectFile::ReadPackageReferences(text))
    {
        if (m_CentralVersions)
            package.Version = SavedVersion(m_VersionsFile, true, package.Id);
        AppendRow(package);
    }
    if (m_CentralVersions)
        CanWriteVersions();
}

void ScriptPackagesList::AddPackage()
{
    if (!m_Rows)
        return;
    const std::string id = m_NewId->GetValue();
    const std::string version = m_NewVersion->GetValue();
    if (!ScriptsProjectFile::IsValidPackageId(id))
    {
        m_Status->SetText("Write the package id as nuget.org shows it, such as Newtonsoft.Json.");
        return;
    }
    if (!ScriptsProjectFile::IsValidPackageVersion(version))
    {
        m_Status->SetText("Write the package version, such as 13.0.3.");
        return;
    }
    String text;
    if (ReadFileTextShared(m_ProjectPath, text))
    {
        for (const ScriptsProjectFile::PackageReference& existing : ScriptsProjectFile::ReadPackageReferences(text))
        {
            if (EqualsIgnoreCase(existing.Id, id))
            {
                m_Status->SetText(existing.Id + " is already in the list: change its version there.");
                return;
            }
        }
    }
    if (m_CentralVersions)
    {
        if (!CanWriteVersions() ||
            !EditFile(m_VersionsFile, [&](std::string& text) { ScriptsProjectFile::SetPackageVersion(text, {id, version}); }) ||
            !EditFile(m_ProjectPath, [&](std::string& text) { ScriptsProjectFile::SetPackageReference(text, {id, {}}); }))
            return;
    }
    else if (!EditFile(m_ProjectPath, [&](std::string& text) { ScriptsProjectFile::SetPackageReference(text, {id, version}); }))
    {
        return;
    }
    AppendRow({id, version});
    m_NewId->SetValue({});
    m_NewVersion->SetValue({});
}

void ScriptPackagesList::RemovePackage(const std::string& id)
{
    UIElement* row = FindRow(id);
    if (!row || !EditFile(m_ProjectPath, [&](std::string& text) { ScriptsProjectFile::RemovePackageReference(text, id); }))
        return;
    m_Rows->RemoveChild(row);
    // The PackageVersion stays: other projects may share Directory.Packages.props, and an unused
    // item restores fine.
    if (m_CentralVersions && !m_VersionsFile.empty())
        m_Status->SetText("Saved to " + m_ProjectPath.filename().string() + ". Its version stays in " +
                          m_VersionsFile.string() + ", which other projects may use.");
    m_Rows->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty | UIElement::ChildrenDirty);
}

void ScriptPackagesList::AppendRow(const ScriptsProjectFile::PackageReference& package)
{
    std::unique_ptr<UIElement> row = Editor::ControlLayoutLoader::Load(kRowLayoutAssetPath, {});
    if (!row)
        return;
    auto* id = dynamic_cast<Label*>(InspectorDrag::FindChildByClass(row.get(), kIdClass));
    auto* version = dynamic_cast<TextField*>(InspectorDrag::FindChildByClass(row.get(), kVersionClass));
    auto* remove = dynamic_cast<Button*>(InspectorDrag::FindChildByClass(row.get(), kRemoveClass));
    if (!id || !version || !remove)
    {
        Logger::Log::Error("Script Packages: {} needs the '{}' label, the '{}' field and the '{}' button.",
                           kRowLayoutAssetPath, kIdClass, kVersionClass, kRemoveClass);
        return;
    }
    id->SetText(package.Id);
    version->SetValue(package.Version);
    version->SetOnValueChanged([this, version, packageId = package.Id](const std::string& value)
                               { ChangeVersion(*version, packageId, value); });
    remove->SetTooltip("Remove this package from the project file");
    // The button attaches the shared button sheet to itself, after this list's; attaching this
    // sheet to it as well lets the remove style outrank the shared button look.
    remove->RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");
    remove->RegisterEventHandler(kEventButtonClick, [this, packageId = package.Id](UIEvent&)
                                 { RemovePackage(packageId); });
    m_Rows->AddChild(std::move(row));
}

void ScriptPackagesList::ChangeVersion(TextField& field, const std::string& id, const std::string& version)
{
    if (!ScriptsProjectFile::IsValidPackageVersion(version))
    {
        m_Status->SetText("'" + version + "' is not a NuGet version. Write one such as 13.0.3, or a range such as [1.0,2.0).");
        // After the message: `version` is the field's own value, which this replaces.
        field.SetValueWithoutNotify(SavedVersion(m_CentralVersions ? m_VersionsFile : m_ProjectPath, m_CentralVersions, id));
        return;
    }
    if (!m_CentralVersions)
        EditFile(m_ProjectPath, [&](std::string& text) { ScriptsProjectFile::SetPackageReference(text, {id, version}); });
    else if (CanWriteVersions())
        EditFile(m_VersionsFile, [&](std::string& text) { ScriptsProjectFile::SetPackageVersion(text, {id, version}); });
}

bool ScriptPackagesList::CanWriteVersions()
{
    if (!m_VersionsFile.empty())
        return true;
    m_Status->SetText("This project sets ManagePackageVersionsCentrally, but no Directory.Packages.props is beside " +
                      m_ProjectPath.filename().string() +
                      " or in a folder above it. Create that file with a PackageVersion item for each package, "
                      "then open this page again.");
    return false;
}

bool ScriptPackagesList::EditFile(const std::filesystem::path& file, const std::function<void(std::string&)>& edit)
{
    String text;
    if (file.empty() || !ReadFileTextShared(file, text))
    {
        if (file == m_ProjectPath)
            m_Status->SetText("The scripts project " + file.string() + " cannot be read. Open the project again to create it.");
        else
            m_Status->SetText("Cannot read " + file.string() + ". Check that it exists and that no program holds it, then try again.");
        return false;
    }
    std::string edited(text);
    edit(edited);
    if (edited == std::string(text))
        return true;
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << edited;
    if (!out)
    {
        Logger::Log::Error("Script Packages: cannot write '{}'", file.string());
        m_Status->SetText("Cannot write " + file.string() +
                          ". Clear its read-only flag or close the program that holds it, then try again.");
        return false;
    }
    m_Status->SetText("Saved to " + file.filename().string() + ". The scripts compile again with the new packages.");
    return true;
}

UIElement* ScriptPackagesList::FindRow(const std::string& id) const
{
    if (!m_Rows)
        return nullptr;
    for (const auto& row : m_Rows->GetChildren())
    {
        const auto* label = dynamic_cast<Label*>(InspectorDrag::FindChildByClass(row.get(), kIdClass));
        if (label && EqualsIgnoreCase(label->GetText(), id))
            return row.get();
    }
    return nullptr;
}

} // namespace GameEngine::Editor
