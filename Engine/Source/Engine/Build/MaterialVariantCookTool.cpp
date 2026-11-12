#include "Engine/Build/MaterialVariantCookTool.h"

#include "AssetCore/SharedFileRead.h"
#include "Core/ApplicationPaths.h"
#include "Engine/Rendering/PackageShaderDirs.h"
#include "FileSystem/FileSystem.h"
#include "Rendering/ShaderCache/ShaderProgramIndex.h"
#include "Types/FlatSet.h"
#include "Types/PathUtils.h"
#include "Types/StringUtils.h"
#include "Types/Types.h"

#include <algorithm>
#include <fstream>
#include <system_error>

namespace GameEngine
{

std::filesystem::path FindMaterialVariantCook()
{
#if defined(_WIN32)
    const std::filesystem::path relative = std::filesystem::path("Tools") / "MaterialVariantCook.exe";
#else
    const std::filesystem::path relative = std::filesystem::path("Tools") / "MaterialVariantCook";
#endif
    return ApplicationPaths::FindFileUpward(ApplicationPaths::GetExecutableDirectory(), relative);
}

std::vector<std::string> DesktopMaterialCookArguments(const DesktopMaterialCookInputs& inputs)
{
    std::vector<std::string> arguments{"--project", inputs.ProjectAssets.string(),
                                       "--engine-shaders", inputs.EngineShaders.string(),
                                       "--cache", inputs.Cache.string(),
                                       "--verify"};
    for (const std::filesystem::path& directory : inputs.PackageShaderDirs)
    {
        arguments.emplace_back("--package-shaders");
        arguments.push_back(directory.string());
    }
    for (const MaterialCookScanRoot& scan : inputs.ScanRoots)
    {
        arguments.emplace_back("--scan");
        arguments.push_back(scan.Path.string());
        arguments.emplace_back("--scan-alias");
        arguments.push_back(scan.Alias);
    }
    if (!inputs.ReuseCache.empty())
    {
        arguments.emplace_back("--reuse-cache");
        arguments.push_back(inputs.ReuseCache.string());
    }
    return arguments;
}

DesktopMaterialCookInputs ExportMaterialCookInputs(const std::filesystem::path& contentRoot,
                                                   const std::filesystem::path& projectRoot,
                                                   const std::vector<MaterialCookScanRoot>& packageRoots)
{
    DesktopMaterialCookInputs inputs;
    inputs.ProjectAssets = contentRoot / "Assets";
    inputs.EngineShaders = inputs.ProjectAssets / "Shaders";
    for (const MaterialCookScanRoot& package : packageRoots)
    {
        std::error_code ec;
        if (std::filesystem::is_directory(package.Path / "Shaders", ec))
            inputs.PackageShaderDirs.push_back(package.Path / "Shaders");
        inputs.ScanRoots.push_back(package);
    }
    AppendStagedModuleShaderDirs(inputs.EngineShaders, inputs.PackageShaderDirs);
    inputs.Cache = projectRoot / ".Build" / "MaterialVariantCook";
    return inputs;
}

namespace
{
constexpr const char* kMaterialCookRecordFileName = "cook-record.txt";
constexpr std::string_view kMaterialCookRecordHeader = "material-cook-record";
constexpr std::string_view kPackageRow = "package";
constexpr std::string_view kFailedRow = "failed";

// `text` with each byte that does not start a well-formed UTF-8 character replaced by U+FFFD.
std::string WithValidUtf8(std::string_view text)
{
    if (IsValidUtf8(text))
        return std::string(text);
    constexpr std::string_view kReplacementCharacter = "\xEF\xBF\xBD";
    constexpr std::size_t kLongestCharacter = 4;
    std::string valid;
    std::size_t at = 0;
    while (at < text.size())
    {
        std::size_t length = 1;
        while (length <= kLongestCharacter && at + length <= text.size() && !IsValidUtf8(text.substr(at, length)))
            ++length;
        if (length > kLongestCharacter || at + length > text.size())
        {
            valid += kReplacementCharacter;
            ++at;
            continue;
        }
        valid += text.substr(at, length);
        at += length;
    }
    return valid;
}

// `text` as one field of a record row: a tab or a line break becomes a space, and the field is UTF-8.
std::string RecordField(std::string text)
{
    std::ranges::replace_if(text, [](char c) { return c == '\t' || c == '\r' || c == '\n'; }, ' ');
    return WithValidUtf8(text);
}

// A record line without the carriage return that a CRLF file adds.
std::string_view WithoutCarriageReturn(std::string_view line)
{
    return line.ends_with('\r') ? line.substr(0, line.size() - 1) : line;
}

// The materials of `failures` that did not load or prepare, in record order, each once:
// `<material>: <reason>`.
std::vector<std::string> UnloadedCookMaterials(const std::vector<MaterialCookFailure>& failures)
{
    std::vector<std::string> materials;
    std::vector<std::string> lines;
    for (const MaterialCookFailure& failure : failures)
    {
        if (!failure.Variant.empty() || std::ranges::find(materials, failure.Material) != materials.end())
            continue;
        materials.push_back(failure.Material);
        lines.push_back(failure.Material + ": " + failure.Reason);
    }
    return lines;
}

// The materials of `failures` with failed variants, in record order, each once with its failed variants:
// `<material>: <variant>, <variant>`.
std::vector<std::string> FailedCookMaterials(const std::vector<MaterialCookFailure>& failures)
{
    std::vector<std::string> materials;
    std::vector<std::vector<std::string>> variants;
    for (const MaterialCookFailure& failure : failures)
    {
        if (failure.Variant.empty())
            continue;
        const auto found = std::ranges::find(materials, failure.Material);
        if (found == materials.end())
        {
            materials.push_back(failure.Material);
            variants.push_back({failure.Variant});
            continue;
        }
        std::vector<std::string>& list = variants[static_cast<std::size_t>(found - materials.begin())];
        if (std::ranges::find(list, failure.Variant) == list.end())
            list.push_back(failure.Variant);
    }
    for (std::size_t i = 0; i < materials.size(); ++i)
        materials[i] += ": " + Join(variants[i], ", ");
    return materials;
}

// Appends `lines` to `message`, one indented line each: at most 20, then the count of the rest.
void AppendListedMaterials(std::string& message, const std::vector<std::string>& lines)
{
    constexpr std::size_t kListedMaterials = 20;
    for (std::size_t i = 0; i < lines.size() && i < kListedMaterials; ++i)
        message += "\n  " + lines[i];
    if (lines.size() > kListedMaterials)
        message += "\n  and " + std::to_string(lines.size() - kListedMaterials) + " more";
}

// The warning of a cook that exited `exitCode` != 0: the failed materials of its record, grouped and
// capped, else what ships and the cook's last output.
std::string DescribeFailedCook(int exitCode, const std::optional<MaterialCookRecord>& record,
                               std::string_view outputTail)
{
    std::string message = "MaterialVariantCook failed (exit " + std::to_string(exitCode) + ")";
    if (!record)
        return message + " before it wrote its record; the export ships no cooked shader package, and the game "
                         "compiles every material on first use:\n" + std::string(outputTail);
    const std::vector<std::string> unloaded = UnloadedCookMaterials(record->Failures);
    const std::vector<std::string> failed = FailedCookMaterials(record->Failures);
    if (unloaded.empty() && failed.empty())
        return message + "; the export ships the variants it cooked, and the game compiles the rest on first "
                         "use:\n" + std::string(outputTail);
    if (!unloaded.empty())
    {
        message += "; the cook could not load or prepare these materials. Fix each material, then export again:";
        AppendListedMaterials(message, unloaded);
    }
    if (!failed.empty())
    {
        message += unloaded.empty() ? "; these" : "\nThese";
        message += " materials compile when the game first draws them:";
        AppendListedMaterials(message, failed);
    }
    return message;
}
} // namespace

bool WriteMaterialCookRecord(const std::filesystem::path& cookCache, const MaterialCookRecord& record,
                             std::string& error)
{
    FlatSet<std::string> listed;
    std::string text = std::string(kMaterialCookRecordHeader) + '\n';
    for (const std::filesystem::path& package : record.Packages)
    {
        const std::string path = RecordField(PathToUtf8(package));
        if (listed.Insert(path))
            text += std::string(kPackageRow) + '\t' + path + '\n';
    }
    for (const MaterialCookFailure& failure : record.Failures)
        text += std::string(kFailedRow) + '\t' + RecordField(failure.Material) + '\t' + RecordField(failure.Variant) +
                '\t' + RecordField(failure.Reason) + '\n';

    const std::filesystem::path path = cookCache / kMaterialCookRecordFileName;
    std::error_code ec;
    std::filesystem::create_directories(cookCache, ec);
    const std::filesystem::path temp = FileSystem::MakeTemporarySiblingPath(path);
    bool written = false;
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        written = static_cast<bool>(out.write(text.data(), static_cast<std::streamsize>(text.size())).flush());
    }
    if (!written)
        std::filesystem::remove(temp, ec);
    if (!written || !FileSystem::PublishFile(temp, path))
    {
        error = "Could not write the material cook record '" + PathToUtf8(path) +
                "'. Free disk space or make the folder writable, then export again.";
        return false;
    }
    return true;
}

std::optional<MaterialCookRecord> ReadMaterialCookRecord(const std::filesystem::path& cookCache)
{
    String text;
    if (!ReadFileTextShared(cookCache / kMaterialCookRecordFileName, text) || !IsValidUtf8(text))
        return std::nullopt;
    const std::vector<std::string_view> lines = Split(text, '\n');
    if (lines.empty() || WithoutCarriageReturn(lines.front()) != kMaterialCookRecordHeader)
        return std::nullopt;
    MaterialCookRecord record;
    for (std::size_t lineIndex = 1; lineIndex < lines.size(); ++lineIndex)
    {
        const std::string_view line = WithoutCarriageReturn(lines[lineIndex]);
        if (line.empty())
            continue;
        const std::vector<std::string_view> fields = Split(line, '\t');
        if (fields.size() == 2 && fields[0] == kPackageRow)
            record.Packages.push_back(PathFromUtf8(fields[1]));
        else if (fields.size() == 4 && fields[0] == kFailedRow)
            record.Failures.push_back({std::string(fields[1]), std::string(fields[2]), std::string(fields[3])});
        else
            return std::nullopt;
    }
    return record;
}

void RemoveMaterialCookOutputs(const std::filesystem::path& cookCache)
{
    std::error_code ec;
    std::filesystem::remove(cookCache / kMaterialCookRecordFileName, ec);
    std::filesystem::remove(cookCache / Rendering::kShaderProgramIndexFileName, ec);
    FileSystem::RemoveOrphanedTemporaryFiles(cookCache, kMaterialCookRecordFileName);
}

MaterialCookOutcome ReadMaterialCookOutcome(const std::filesystem::path& cookCache, int exitCode,
                                            std::string_view outputTail)
{
    MaterialCookOutcome outcome;
    outcome.Record = ReadMaterialCookRecord(cookCache);
    if (exitCode != 0)
    {
        outcome.Warning = DescribeFailedCook(exitCode, outcome.Record, outputTail);
        return outcome;
    }
    if (!outcome.Record)
        outcome.Error = "MaterialVariantCook exited 0 and wrote no readable record '" +
                        PathToUtf8(cookCache / kMaterialCookRecordFileName) +
                        "', so the export cannot tell which shader packages to ship. The cook tool does not match "
                        "this Editor: build the Editor target again, which builds the tool, then export.";
    return outcome;
}

bool StageCookedShaderPackages(const std::vector<std::filesystem::path>& packages,
                               const std::filesystem::path& cookCache, const std::filesystem::path& bundleCache,
                               std::string& error)
{
    const std::filesystem::path cacheRoot = cookCache.lexically_normal();
    for (const std::filesystem::path& package : packages)
    {
        const std::filesystem::path relative = package.lexically_normal().lexically_relative(cacheRoot);
        if (relative.empty() || *relative.begin() == "..")
        {
            error = "The material cook named the shader package '" + PathToUtf8(package) +
                    "', which is outside its cache '" + PathToUtf8(cookCache) +
                    "'. Build the Editor target again, which builds the cook tool, then export.";
            return false;
        }
        std::error_code ec;
        if (!std::filesystem::is_regular_file(package, ec))
        {
            error = "The material cook listed the shader package '" + PathToUtf8(package) +
                    "', which is not on disk. Export again to cook it.";
            return false;
        }
        const std::filesystem::path destination = bundleCache / relative;
        std::filesystem::create_directories(destination.parent_path(), ec);
        if (!ec)
            std::filesystem::copy_file(package, destination, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
        {
            error = "Could not copy the cooked shader package '" + PathToUtf8(package) + "' to '" +
                    PathToUtf8(destination) + "': " + ec.message() +
                    ". Free disk space or make the export folder writable, then export again.";
            return false;
        }
    }
    // The program index names the packages of the variants this cook listed, so it ships with them.
    const std::filesystem::path index = cookCache / Rendering::kShaderProgramIndexFileName;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(index, ec))
        return true;
    std::filesystem::create_directories(bundleCache, ec);
    if (!ec)
        std::filesystem::copy_file(index, bundleCache / Rendering::kShaderProgramIndexFileName,
                                   std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
    {
        error = "Could not copy the material cook's program index '" + PathToUtf8(index) + "' to '" +
                PathToUtf8(bundleCache) + "': " + ec.message() +
                ". Free disk space or make the export folder writable, then export again.";
        return false;
    }
    return true;
}

namespace
{
// True when @p folder is @p root or a folder inside it.
bool IsSameOrInside(const std::filesystem::path& folder, const std::filesystem::path& root)
{
    const std::filesystem::path relative = folder.lexically_normal().lexically_relative(root.lexically_normal());
    return !relative.empty() && *relative.begin() != "..";
}
} // namespace

std::vector<std::filesystem::path> DesktopShaderPackageRoots(const DesktopMaterialCookInputs& inputs)
{
    std::vector<std::filesystem::path> candidates{inputs.EngineShaders};
    candidates.insert(candidates.end(), inputs.PackageShaderDirs.begin(), inputs.PackageShaderDirs.end());
    candidates.push_back(inputs.Cache);
    candidates.push_back(inputs.ReuseCache);
    std::vector<std::filesystem::path> roots;
    for (const std::filesystem::path& candidate : candidates)
    {
        if (candidate.empty() ||
            std::ranges::any_of(roots, [&](const std::filesystem::path& root) { return IsSameOrInside(candidate, root); }))
            continue;
        std::erase_if(roots, [&](const std::filesystem::path& root) { return IsSameOrInside(root, candidate); });
        roots.push_back(candidate);
    }
    return roots;
}

} // namespace GameEngine
