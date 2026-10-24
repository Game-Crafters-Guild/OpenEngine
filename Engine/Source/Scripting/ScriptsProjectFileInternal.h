#pragma once

// The scripts csproj functions only the script manager and the package restore use (the public
// ones, for the settings page, are in Scripting/ScriptsProjectFile.h). Like those, each works on
// the file's text and leaves everything it does not name byte for byte as it was.

#include <string>
#include <string_view>

namespace GameEngine::ScriptsProjectFile
{

inline constexpr std::string_view kEnginePropsFileName = "GameEngine.Scripts.Engine.props";
inline constexpr std::string_view kEngineTargetsFileName = "GameEngine.Scripts.Engine.targets";

/// The csproj the editor writes when the project has none.
std::string NewProjectText();

/// What ReconcileEngineImports found and changed.
struct ReconcileResult
{
    bool PropsImportAdded = false;
    bool TargetsImportAdded = false;
    bool SdkMissing = false; // the Project element has no Sdk attribute; never edited
};

/// Puts back a missing import of the engine props (after the opening Project tag) or the
/// engine targets (before the closing tag). Nothing else in `text` changes.
ReconcileResult ReconcileEngineImports(std::string& text);

/// True when the csproj declares a PackageReference, Reference or ProjectReference item outside
/// a comment: the items a restore has to resolve.
bool DeclaresReferenceItems(std::string_view text);

/// True when the text sets ManagePackageVersionsCentrally to true outside a comment.
bool SetsCentralPackageVersions(std::string_view text);

} // namespace GameEngine::ScriptsProjectFile
