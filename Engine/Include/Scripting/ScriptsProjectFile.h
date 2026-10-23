#pragma once

// The project's scripts csproj (GameEngine.Scripts.csproj) belongs to the user: the editor
// creates it once and then edits it only to put back a missing engine import or to add,
// change or remove a PackageReference from the Script Packages settings page. Under central
// package versions the page writes the versions as PackageVersion items of
// Directory.Packages.props. The engine's own settings live in two files the editor rewrites on
// every load and the csproj imports: GameEngine.Scripts.Engine.props (target framework, output,
// the script source glob, the engine references) and GameEngine.Scripts.Engine.targets
// (diagnostics, validation and the staging of package assemblies into Dependencies/).
//
// Every function here works on the file's text and leaves everything it does not name
// byte for byte as it was, so the user's IDE and the settings page see one file.

#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::ScriptsProjectFile
{

/// One `<PackageReference Include="Id" Version="Version" />` of the csproj.
struct PackageReference
{
    std::string Id;
    std::string Version; // empty when the element declares none (central package versions)
};

/// The PackageReference items of the csproj, in file order. Commented-out items are skipped.
std::vector<PackageReference> ReadPackageReferences(std::string_view text);

/// Adds the package, or changes the version of the item with the same id (ids compare
/// without case, as NuGet does). A new item goes after the last existing PackageReference,
/// or into a new ItemGroup before the engine targets import. An empty version writes an item
/// without a Version, as central package versions need, and leaves an existing item as it is.
void SetPackageReference(std::string& text, const PackageReference& package);

/// Removes the item with this id; false when the csproj has none.
bool RemovePackageReference(std::string& text, std::string_view id);

/// One `<PackageVersion Include="Id" Version="Version" />` of a Directory.Packages.props.
struct PackageVersion
{
    std::string Id;
    std::string Version;
};

/// The PackageVersion items of a Directory.Packages.props, in file order. Commented-out items
/// are skipped.
std::vector<PackageVersion> ReadPackageVersions(std::string_view text);

/// Adds the PackageVersion, or changes the version of the item with the same id. A new item goes
/// after the last existing PackageVersion, or into a new ItemGroup before the closing Project tag.
void SetPackageVersion(std::string& text, const PackageVersion& package);

/// NuGet package id characters: letters, digits, '.', '_' and '-', starting with a letter,
/// a digit or '_'.
bool IsValidPackageId(std::string_view id);

/// A NuGet version or version range: digits, letters, '.', '-', '+', '*', ',', and range
/// brackets. Empty is not valid.
bool IsValidPackageVersion(std::string_view version);

} // namespace GameEngine::ScriptsProjectFile
