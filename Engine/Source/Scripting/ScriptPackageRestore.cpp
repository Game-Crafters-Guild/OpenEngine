#include "Scripting/ScriptPackageRestore.h"

#include "ScriptPackageRestoreInternal.h"
#include "ScriptsProjectFileInternal.h"

#include "AssetCore/SharedFileRead.h"
#include "Engine/Build/CancellableShellProcess.h"
#include "Engine/Build/DotnetHost.h"
#include "Logger/Logger.h"
#include "Platform/Process.h"
#include "Scripting/ScriptTargetFramework.h"
#include "Scripting/ScriptsProjectFile.h"
#include "Types/Fnv1a.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

namespace GameEngine
{
namespace
{
// Beside the restore's own project.assets.json in the project's obj/ folder: the key of the
// inputs the last successful restore ran on (first line), then the assemblies it resolved, one
// absolute path per line.
constexpr std::string_view kRestoreRecordFileName = "GameEngine.Scripts.restore";

// Beside the staged assemblies in Dependencies/: what each was copied from (StageAssemblies).
constexpr std::string_view kStagedManifestFileName = "GameEngine.Dependencies.staged";

// The file that holds the PackageVersion items under central package versions.
constexpr std::string_view kCentralVersionsFileName = "Directory.Packages.props";

// The file names, other than the csproj, that change what a restore resolves.
constexpr std::array<std::string_view, 4> kRestoreInputFileNames = {
    "Directory.Build.props", "Directory.Build.targets", kCentralVersionsFileName, "nuget.config"};

// A restore that reaches a stalled feed or waits on a credential stops after this long.
constexpr std::chrono::minutes kRestoreTimeLimit{5};

std::string ReadText(const std::filesystem::path& path)
{
    String text;
    if (!ReadFileTextShared(path, text))
        return {};
    return std::string(text);
}

void WriteText(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream of(path, std::ios::binary | std::ios::trunc);
    of << text;
}

// The error lines of a failed restore or evaluation, without the csproj path MSBuild puts in
// front of each.
std::string ProcessErrors(const std::string& output)
{
    std::istringstream stream(output);
    std::string line;
    std::set<std::string> seen;
    std::string errors;
    while (std::getline(stream, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const std::size_t marker = line.find(": error ");
        if (marker == std::string::npos)
            continue;
        std::string message = line.substr(marker + 2);
        // MSBuild repeats the project path in brackets at the end of the line.
        if (const std::size_t bracket = message.rfind(" ["); bracket != std::string::npos && message.back() == ']')
            message.erase(bracket);
        if (seen.insert(message).second)
            errors += "  " + message + "\n";
    }
    return errors;
}

// What to do about a failed restore, from the NuGet codes its errors carry: NU1101 to NU1103 name
// a package or a version no source has; NU1301 and NU1801 a source that did not answer.
std::string RestoreFix(const std::string& errors)
{
    const bool unknownPackage = errors.find("NU1101") != std::string::npos ||
                                errors.find("NU1102") != std::string::npos ||
                                errors.find("NU1103") != std::string::npos;
    const bool sourceFailed =
        errors.find("NU1301") != std::string::npos || errors.find("NU1801") != std::string::npos;
    std::string fix;
    if (unknownPackage)
        fix += "Fix the package id or version in the PackageReference items of that file (or on the Script "
               "Packages settings page), then save to compile again.";
    if (sourceFailed)
        fix += std::string(fix.empty() ? "" : " ") +
               "A package source did not answer: check the network connection and the sources and their "
               "credentials in nuget.config, then save to compile again.";
    if (fix.empty())
        fix = "Fix what the errors name in that file, then save to compile again.";
    return fix;
}

// Runs dotnet with `arguments` until it exits, `cancelRequested` is set or the time limit passes.
ShellProcessResult RunDotnet(const std::vector<std::string>& arguments, const std::atomic<bool>* cancelRequested,
                             bool& outTimedOut)
{
    ProcessLaunch launch;
    launch.Executable = DotnetHostCommand();
    launch.Arguments = arguments;
    const auto deadline = std::chrono::steady_clock::now() + kRestoreTimeLimit;
    outTimedOut = false;
    std::string output;
    CancellableShellProcess process;
    ShellProcessResult result = process.Run(
        launch,
        [&]()
        {
            if (std::chrono::steady_clock::now() >= deadline)
                outTimedOut = true;
            return outTimedOut || (cancelRequested && cancelRequested->load(std::memory_order_relaxed));
        },
        [&](const std::string& line) { output += line + "\n"; });
    result.output = std::move(output) + result.stderrTail;
    return result;
}

// The message of a restore step that did not finish: stopped by a newer compile or by the time limit.
std::string StoppedMessage(const std::filesystem::path& projectPath, bool timedOut)
{
    if (timedOut)
        return "The restore of the script packages of '" + projectPath.string() + "' did not finish within " +
               std::to_string(kRestoreTimeLimit.count()) +
               " minutes and was stopped. Check the network connection and the package sources and their "
               "credentials in nuget.config, then save to compile again.";
    return "The restore of the script packages of '" + projectPath.string() + "' was stopped: a newer compile "
           "replaced this one.";
}

// The key of every restore input: each file's path and text, and the engine folder the engine
// props resolve their references from.
std::string RestoreKey(const std::vector<std::filesystem::path>& inputs, const std::string& engineBinDir)
{
    std::uint64_t hash = Hashing::Fnv1a64(engineBinDir);
    for (const std::filesystem::path& input : inputs)
    {
        hash = Hashing::Fnv1a64(input.generic_string(), hash);
        hash = Hashing::Fnv1a64(std::string_view("\n"), hash);
        hash = Hashing::Fnv1a64(ReadText(input), hash);
        hash = Hashing::Fnv1a64(std::string_view("\n"), hash);
    }
    return std::to_string(hash);
}

// The record of the last successful restore: false when there is none.
bool ReadRestoreRecord(const std::filesystem::path& recordPath, std::string& outKey,
                       std::vector<std::filesystem::path>& outAssemblies)
{
    std::istringstream stream(ReadText(recordPath));
    if (!std::getline(stream, outKey) || outKey.empty())
        return false;
    std::string line;
    while (std::getline(stream, line))
    {
        if (!line.empty())
            outAssemblies.emplace_back(line);
    }
    return true;
}

// True when every assembly the record names is still on disk. A cleared package cache or a deleted
// HintPath file makes the record stale, and the restore runs again.
bool RecordedAssembliesExist(const std::vector<std::filesystem::path>& assemblies)
{
    std::error_code ec;
    return std::all_of(assemblies.begin(), assemblies.end(),
                       [&ec](const std::filesystem::path& assembly) { return std::filesystem::exists(assembly, ec); });
}

void WriteRestoreRecord(const std::filesystem::path& recordPath, const std::string& key,
                        const std::vector<std::filesystem::path>& assemblies)
{
    std::string text = key + "\n";
    for (const std::filesystem::path& assembly : assemblies)
        text += assembly.string() + "\n";
    WriteText(recordPath, text);
}

// The file name as UTF-8: unlike string(), it never throws for a name outside the code page.
std::string FileNameUtf8(const std::filesystem::path& file)
{
    const std::u8string name = file.filename().u8string();
    return std::string(name.begin(), name.end());
}

bool IsRestoreInputFileName(std::string_view fileName)
{
    return std::any_of(kRestoreInputFileNames.begin(), kRestoreInputFileNames.end(),
                       [fileName](std::string_view name) { return EqualsIgnoreCase(name, fileName); });
}

// True when an input other than the csproj and the engine's own files is an MSBuild file, which
// can declare reference items the csproj text does not show.
bool HasDirectoryMsBuildFile(const std::vector<std::filesystem::path>& inputs)
{
    return std::any_of(inputs.begin(), inputs.end(),
                       [](const std::filesystem::path& input)
                       {
                           const std::string name = FileNameUtf8(input);
                           return IsRestoreInputFileName(name) && !EqualsIgnoreCase(name, "nuget.config");
                       });
}

// Where a staged file was copied from: the source's path, size and write time. Two versions of a
// package can share a size and a write time (the package format keeps times to two seconds), but
// never a path.
std::string SourceStamp(const std::filesystem::path& source)
{
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(source, ec);
    const auto time = static_cast<long long>(std::filesystem::last_write_time(source, ec).time_since_epoch().count());
    return source.string() + "|" + std::to_string(size) + "|" + std::to_string(time);
}

// The staged manifest: file name to SourceStamp, one "name<TAB>stamp" line each.
std::map<std::string, std::string> ReadStagedManifest(const std::filesystem::path& manifestPath)
{
    std::map<std::string, std::string> manifest;
    std::istringstream stream(ReadText(manifestPath));
    std::string line;
    while (std::getline(stream, line))
    {
        if (const std::size_t tab = line.find('\t'); tab != std::string::npos)
            manifest[line.substr(0, tab)] = line.substr(tab + 1);
    }
    return manifest;
}

// Copies `assemblies` into `dependenciesDir` (only those whose source changed since they were
// staged) and deletes every other assembly there, so the folder holds exactly the restored set. A
// copy gets a new write time, so the scripts' dependency context, which fingerprints the folder by
// names, sizes and write times, loads the new set.
bool StageAssemblies(const std::vector<std::filesystem::path>& assemblies,
                     const std::filesystem::path& dependenciesDir, std::vector<std::filesystem::path>& staged,
                     std::string& error)
{
    std::error_code ec;
    std::filesystem::create_directories(dependenciesDir, ec);
    const std::filesystem::path manifestPath = dependenciesDir / kStagedManifestFileName;
    const std::map<std::string, std::string> previous = ReadStagedManifest(manifestPath);
    std::map<std::string, std::string> manifest;
    for (const std::filesystem::path& source : assemblies)
    {
        const std::filesystem::path target = dependenciesDir / source.filename();
        const std::string name = source.filename().string();
        const std::string stamp = SourceStamp(source);
        manifest[name] = stamp;
        const auto recorded = previous.find(name);
        const bool current = std::filesystem::exists(target, ec) && recorded != previous.end() && recorded->second == stamp;
        if (!current)
        {
            std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec)
            {
                error = "Could not copy the package assembly '" + source.string() + "' into '" +
                        dependenciesDir.string() + "' (" + ec.message() +
                        "). Close the program that holds the file (a running game or the IDE), then compile again.";
                return false;
            }
            std::filesystem::last_write_time(target, std::filesystem::file_time_type::clock::now(), ec);
        }
        staged.push_back(target);
    }
    for (const auto& entry : std::filesystem::directory_iterator(dependenciesDir, ec))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".dll" &&
            manifest.find(entry.path().filename().string()) == manifest.end())
            std::filesystem::remove(entry.path(), ec);
    }
    std::string manifestText;
    for (const auto& [name, stamp] : manifest)
        manifestText += name + "\t" + stamp + "\n";
    WriteText(manifestPath, manifestText);
    std::sort(staged.begin(), staged.end(),
              [](const std::filesystem::path& a, const std::filesystem::path& b) { return a.filename() < b.filename(); });
    return true;
}

// The first native or platform-specific asset of a package's target entry, or empty when it has
// none. "_._" marks a folder the package leaves empty on purpose.
std::string FirstUnstagedAsset(const nlohmann::ordered_json& entry)
{
    for (const char* group : {"native", "runtimeTargets"})
    {
        const auto assets = entry.find(group);
        if (assets == entry.end() || !assets->is_object())
            continue;
        for (auto asset = assets->begin(); asset != assets->end(); ++asset)
        {
            if (std::filesystem::path(asset.key()).filename() != "_._")
                return asset.key();
        }
    }
    return {};
}

// True for a Reference item the engine props or targets declare: the engine's own assemblies,
// which every compile references already.
bool IsEngineItem(const nlohmann::json& item)
{
    const std::string definedBy =
        std::filesystem::path(item.value("DefiningProjectFullPath", std::string())).filename().string();
    return EqualsIgnoreCase(definedBy, ScriptsProjectFile::kEnginePropsFileName) ||
           EqualsIgnoreCase(definedBy, ScriptsProjectFile::kEngineTargetsFileName);
}
} // namespace

bool IsScriptRestoreInput(const std::filesystem::path& file, const std::filesystem::path& projectPath)
{
    const std::string name = FileNameUtf8(file);
    return EqualsIgnoreCase(name, FileNameUtf8(projectPath)) || IsRestoreInputFileName(name);
}

std::vector<std::filesystem::path> ScriptRestoreInputFiles(const std::filesystem::path& projectPath)
{
    std::vector<std::filesystem::path> inputs{projectPath};
    std::error_code ec;
    const std::filesystem::path projectDir = std::filesystem::absolute(projectPath, ec).parent_path();
    for (const std::string_view engineFile :
         {ScriptsProjectFile::kEnginePropsFileName, ScriptsProjectFile::kEngineTargetsFileName})
    {
        const std::filesystem::path path = projectDir / engineFile;
        if (std::filesystem::is_regular_file(path, ec))
            inputs.push_back(path);
    }
    for (std::filesystem::path dir = projectDir; !dir.empty(); dir = dir.parent_path())
    {
        std::vector<std::filesystem::path> found;
        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        {
            if (IsRestoreInputFileName(FileNameUtf8(it->path())) && it->is_regular_file(ec))
                found.push_back(it->path());
        }
        ec.clear();
        std::sort(found.begin(), found.end());
        inputs.insert(inputs.end(), found.begin(), found.end());
        if (dir.parent_path() == dir)
            break;
    }
    return inputs;
}

bool ReadRestoredRuntimeAssemblies(std::string_view assetsJson, std::string_view targetFramework,
                                   std::vector<std::filesystem::path>& outAssemblies,
                                   std::vector<std::string>& outWarnings, std::string& error)
{
    // Ordered: the restore lists the user's package folder first and fallback folders after it.
    const nlohmann::ordered_json assets = nlohmann::ordered_json::parse(assetsJson, nullptr, /*allow_exceptions=*/false);
    if (!assets.is_object() || !assets.contains("targets") || !assets.contains("libraries") ||
        !assets.contains("packageFolders"))
    {
        error = "project.assets.json is not a restore result";
        return false;
    }
    const nlohmann::ordered_json& targets = assets["targets"];
    const auto target = targets.find(std::string(targetFramework));
    if (target == targets.end() || !target->is_object())
    {
        error = "project.assets.json has no target for " + std::string(targetFramework);
        return false;
    }
    const nlohmann::ordered_json& folders = assets["packageFolders"];
    if (!folders.is_object() || folders.empty())
    {
        error = "project.assets.json names no package folder";
        return false;
    }
    std::vector<std::filesystem::path> packageFolders;
    for (auto folder = folders.begin(); folder != folders.end(); ++folder)
        packageFolders.emplace_back(folder.key());
    const nlohmann::ordered_json& libraries = assets["libraries"];

    for (auto it = target->begin(); it != target->end(); ++it)
    {
        const nlohmann::ordered_json& entry = it.value();
        if (entry.value("type", std::string()) != "package")
            continue;
        if (const std::string asset = FirstUnstagedAsset(entry); !asset.empty())
        {
            std::string package = it.key();
            std::replace(package.begin(), package.end(), '/', ' ');
            outWarnings.push_back("The script package " + package + " has the native or platform-specific asset '" +
                                  asset +
                                  "', which is not supported: assemblies load from bytes. The package's code that "
                                  "needs the asset fails when it runs.");
        }
        if (!entry.contains("runtime"))
            continue;
        const auto library = libraries.find(it.key());
        if (library == libraries.end() || !library->contains("path"))
            continue;
        const std::string libraryPath = (*library)["path"].get<std::string>();
        for (auto runtime = entry["runtime"].begin(); runtime != entry["runtime"].end(); ++runtime)
        {
            const std::filesystem::path relative(runtime.key());
            // "_._" marks a framework the package deliberately leaves empty.
            if (relative.filename() == "_._" || relative.extension() != ".dll")
                continue;
            // The first folder that holds the package, as NuGet resolves it; the user's folder
            // when none does yet (the error then names that path).
            std::filesystem::path resolved = (packageFolders.front() / libraryPath / relative).lexically_normal();
            for (const std::filesystem::path& folder : packageFolders)
            {
                std::error_code ec;
                const std::filesystem::path candidate = (folder / libraryPath / relative).lexically_normal();
                if (std::filesystem::exists(candidate, ec))
                {
                    resolved = candidate;
                    break;
                }
            }
            outAssemblies.push_back(resolved);
        }
    }
    std::sort(outAssemblies.begin(), outAssemblies.end(),
              [](const std::filesystem::path& a, const std::filesystem::path& b) { return a.filename() < b.filename(); });
    return true;
}

bool ReadEvaluatedReferences(std::string_view evaluationJson, std::vector<std::filesystem::path>& outReferences,
                             std::vector<std::string>& outWarnings, std::string& error)
{
    const nlohmann::json evaluation = nlohmann::json::parse(evaluationJson, nullptr, /*allow_exceptions=*/false);
    if (!evaluation.is_object() || !evaluation.contains("Items") || !evaluation["Items"].is_object())
    {
        error = "the project's evaluation printed no item list";
        return false;
    }
    const nlohmann::json& items = evaluation["Items"];
    if (const auto references = items.find("Reference"); references != items.end() && references->is_array())
    {
        for (const nlohmann::json& item : *references)
        {
            if (!item.is_object() || IsEngineItem(item))
                continue;
            const std::string identity = item.value("Identity", std::string());
            std::filesystem::path file(item.value("HintPath", std::string()));
            if (file.empty())
            {
                // An Include that is itself a path names the file; a bare name is a framework assembly.
                if (!EndsWithIgnoreCase(identity, ".dll"))
                    continue;
                file = item.value("FullPath", std::string());
            }
            if (file.is_relative())
                file = std::filesystem::path(item.value("DefiningProjectDirectory", std::string())) / file;
            file = file.lexically_normal();
            std::error_code ec;
            if (!std::filesystem::is_regular_file(file, ec))
            {
                error = "the Reference '" + identity + "' of '" + item.value("DefiningProjectFullPath", std::string()) +
                        "' names '" + file.string() + "', which does not exist. Build that assembly or fix the HintPath";
                return false;
            }
            outReferences.push_back(file);
        }
    }
    if (const auto projects = items.find("ProjectReference"); projects != items.end() && projects->is_array())
    {
        for (const nlohmann::json& item : *projects)
        {
            if (!item.is_object())
                continue;
            outWarnings.push_back("The scripts project references the project '" +
                                  item.value("FullPath", std::string()) +
                                  "', which the editor's compile does not build, so its types are missing there. "
                                  "Build that project and add a Reference to its output assembly instead.");
        }
    }
    return true;
}

CentralPackageVersions FindCentralPackageVersions(const std::filesystem::path& projectPath)
{
    CentralPackageVersions result;
    for (const std::filesystem::path& input : ScriptRestoreInputFiles(projectPath))
    {
        if (ScriptsProjectFile::SetsCentralPackageVersions(ReadText(input)))
            result.Enabled = true;
        if (result.File.empty() && EqualsIgnoreCase(FileNameUtf8(input), kCentralVersionsFileName))
            result.File = input;
    }
    return result;
}

ScriptPackageRestoreResult RestoreScriptPackages(const std::filesystem::path& projectPath,
                                                 const std::filesystem::path& dependenciesDir,
                                                 const std::string& engineBinDir,
                                                 const std::atomic<bool>* cancelRequested)
{
    ScriptPackageRestoreResult result;
    const std::vector<std::filesystem::path> inputs = ScriptRestoreInputFiles(projectPath);
    if (!ScriptsProjectFile::DeclaresReferenceItems(ReadText(projectPath)) && !HasDirectoryMsBuildFile(inputs))
    {
        std::vector<std::filesystem::path> none;
        result.Success = StageAssemblies({}, dependenciesDir, none, result.Error);
        return result;
    }

    const std::filesystem::path objDir = projectPath.parent_path() / "obj";
    const std::filesystem::path assetsPath = objDir / "project.assets.json";
    const std::filesystem::path recordPath = objDir / kRestoreRecordFileName;
    const std::string key = RestoreKey(inputs, engineBinDir);
    std::string recordedKey;
    std::vector<std::filesystem::path> assemblies;
    std::error_code ec;
    if (!std::filesystem::exists(assetsPath, ec) || !ReadRestoreRecord(recordPath, recordedKey, assemblies) ||
        recordedKey != key || !RecordedAssembliesExist(assemblies))
    {
        assemblies.clear();
        Logger::Log::Info("Restoring the script packages of '{}'", projectPath.string());
        const std::string engineBinProperty = "-p:EngineBinDir=" + engineBinDir;
        bool timedOut = false;
        const ShellProcessResult restore = RunDotnet(
            {"restore", projectPath.string(), "--verbosity", "minimal", "--interactive", "false", engineBinProperty},
            cancelRequested, timedOut);
        if (restore.cancelled)
        {
            result.Stopped = !timedOut;
            result.Error = StoppedMessage(projectPath, timedOut);
            return result;
        }
        if (restore.exitCode != 0)
        {
            std::string errors = ProcessErrors(restore.output);
            if (errors.empty())
                errors = "  dotnet restore exited with code " + std::to_string(restore.exitCode) + "\n";
            result.Error = "The script packages of '" + projectPath.string() + "' did not restore:\n" + errors +
                           RestoreFix(errors);
            return result;
        }
        if (!ReadRestoredRuntimeAssemblies(ReadText(assetsPath), kScriptTargetFramework, assemblies, result.Warnings,
                                           result.Error))
        {
            result.Error = "The script packages of '" + projectPath.string() + "' restored, but " + result.Error +
                           ". Delete the obj folder beside that file and compile again.";
            return result;
        }

        // The Reference and ProjectReference items as MSBuild evaluates them, so a condition or a
        // Directory.Build.props item counts as it does in the IDE.
        const ShellProcessResult evaluation =
            RunDotnet({"msbuild", projectPath.string(), "-nologo", "-getItem:Reference", "-getItem:ProjectReference",
                       engineBinProperty},
                      cancelRequested, timedOut);
        if (evaluation.cancelled)
        {
            result.Stopped = !timedOut;
            result.Error = StoppedMessage(projectPath, timedOut);
            return result;
        }
        std::vector<std::filesystem::path> references;
        std::string referenceError;
        if (evaluation.exitCode != 0)
            referenceError = ProcessErrors(evaluation.output) + "  dotnet msbuild exited with code " +
                             std::to_string(evaluation.exitCode);
        if (!referenceError.empty() ||
            !ReadEvaluatedReferences(evaluation.output, references, result.Warnings, referenceError))
        {
            result.Error = "The references of '" + projectPath.string() + "' did not resolve: " + referenceError +
                           ". Fix that in the file, then save to compile again.";
            return result;
        }
        assemblies.insert(assemblies.end(), references.begin(), references.end());
        std::filesystem::create_directories(objDir, ec);
        WriteRestoreRecord(recordPath, key, assemblies);
    }
    result.Success = StageAssemblies(assemblies, dependenciesDir, result.StagedAssemblies, result.Error);
    return result;
}

} // namespace GameEngine
