#include "Assets/AssetRename.h"

#include "AssetCore/AssetTypes.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Logger/Logger.h"
#include "Types/StringUtils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <functional>
#include <string_view>
#include <system_error>

namespace GameEngine::Editor
{

namespace
{

constexpr std::size_t kMaxFileNameBytes = 255;
constexpr std::string_view kForbiddenNameChars = "/\\:*?\"<>|";

// Names no Windows volume accepts, with or without an extension.
constexpr std::array<std::string_view, 22> kWindowsReservedNames = {
    "CON", "PRN", "AUX", "NUL",
    "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
    "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
};

bool IsShaderPath(const std::filesystem::path& path)
{
    return GetAssetTypeFromExtension(ToLowerAscii(path.extension().string())) == AssetType::Shader;
}

bool FilenameMatches(const std::string& reference, const std::string& filename)
{
    if (reference.empty())
        return false;
    return ToLowerAscii(std::filesystem::path(reference).filename().string()) == ToLowerAscii(filename);
}

bool ReadFile(const std::filesystem::path& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
        return false;
    out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return true;
}

// What a rename keeps after the edited name: a file's extension; nothing for a folder, whose
// whole name is edited.
std::string KeptSuffix(const std::filesystem::path& path)
{
    std::error_code ec;
    return std::filesystem::is_directory(path, ec) ? std::string{} : path.extension().string();
}

} // namespace

std::string AssetRenameEditableName(const std::filesystem::path& path)
{
    std::error_code ec;
    return std::filesystem::is_directory(path, ec) ? path.filename().string() : path.stem().string();
}

AssetRenameValidation ValidateAssetRename(const std::string& newName, const std::filesystem::path& currentPath)
{
    if (newName.empty())
        return {false, "Name can't be empty"};

    const bool allSpace = std::all_of(newName.begin(), newName.end(), [](unsigned char c) { return std::isspace(c); });
    if (allSpace)
        return {false, "Name can't be only spaces"};

    for (unsigned char c : newName)
    {
        if (std::iscntrl(c))
            return {false, "Name can't contain control characters"};
        if (kForbiddenNameChars.find(static_cast<char>(c)) != std::string_view::npos)
            return {false, "Name can't contain any of / \\ : * ? \" < > |"};
    }

    if (newName.front() == '.')
        return {false, "Name can't start with a dot"};
    if (std::isspace(static_cast<unsigned char>(newName.front())))
        return {false, "Name can't start with a space"};
    if (newName.back() == '.' || std::isspace(static_cast<unsigned char>(newName.back())))
        return {false, "Name can't end with a space or a dot"};

    const std::string upper = ToUpperAscii(newName);
    for (std::string_view reserved : kWindowsReservedNames)
    {
        if (upper == reserved)
            return {false, "'" + newName + "' is a reserved name on Windows"};
    }

    const std::string extension = KeptSuffix(currentPath);
    if (newName.size() + extension.size() > kMaxFileNameBytes)
        return {false, "Name is too long"};

    const std::filesystem::path target = currentPath.parent_path() / (newName + extension);
    std::error_code ec;
    if (std::filesystem::exists(target, ec) && !std::filesystem::equivalent(target, currentPath, ec))
        return {false, "'" + target.filename().string() + "' already exists here. Choose another name."};

    if (IsShaderPath(currentPath))
    {
        const std::filesystem::path companion = FindPairedSurfaceShaderMaterial(currentPath);
        if (!companion.empty())
        {
            const std::filesystem::path companionTarget =
                companion.parent_path() / (newName + companion.extension().string());
            if (std::filesystem::exists(companionTarget, ec) &&
                !std::filesystem::equivalent(companionTarget, companion, ec))
                return {false, "'" + companionTarget.filename().string() + "' already exists here. Choose another name."};
        }
    }

    return {true, {}};
}

std::filesystem::path FindPairedSurfaceShaderMaterial(const std::filesystem::path& shaderPath)
{
    if (shaderPath.empty() || !IsShaderPath(shaderPath))
        return {};

    const std::filesystem::path candidate = shaderPath.parent_path() / (shaderPath.stem().string() + ".material");
    std::error_code ec;
    if (!std::filesystem::is_regular_file(candidate, ec))
        return {};

    std::string text;
    if (!ReadFile(candidate, text))
        return {};

    const nlohmann::json doc = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!doc.is_object())
        return {};

    const std::string surfaceShader = doc.value("surfaceShader", std::string());
    return FilenameMatches(surfaceShader, shaderPath.filename().string()) ? candidate : std::filesystem::path{};
}

namespace
{

// Applies `edit` to the material document and writes it back; false when the file is
// unreadable, not a JSON object, or `edit` declined to change it.
bool RewriteMaterialDocument(const std::filesystem::path& materialPath,
                             const std::function<bool(nlohmann::json&)>& edit)
{
    std::string text;
    if (!ReadFile(materialPath, text))
        return false;

    nlohmann::json doc = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!doc.is_object() || !edit(doc))
        return false;

    std::ofstream out(materialPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open())
    {
        Logger::Log::Warning("AssetRename: cannot rewrite '{}'", materialPath.string());
        return false;
    }
    out << doc.dump(2) << "\n";
    return true;
}

} // namespace

bool RewriteMaterialShaderReferences(const std::filesystem::path& materialPath,
                                     const std::string& oldShaderFilename,
                                     const std::string& newShaderFilename)
{
    // Every reference a material can hold to one shader file: the path key and
    // the GUID key stamped beside it. The inspector points both pairs at the
    // same file when it carries the surface and the vertex-modifier stage.
    struct ShaderReference
    {
        const char* PathKey;
        const char* GuidKey;
    };
    constexpr std::array<ShaderReference, 2> kShaderReferences{{
        {"surfaceShader", "surfaceShaderGuid"},
        {"vertexModifier", "vertexModifierGuid"},
    }};

    return RewriteMaterialDocument(materialPath, [&](nlohmann::json& doc)
    {
        bool rewritten = false;
        for (const ShaderReference& reference : kShaderReferences)
        {
            const std::string current = doc.value(reference.PathKey, std::string());
            if (!FilenameMatches(current, oldShaderFilename))
                continue;
            std::filesystem::path path(current);
            path.replace_filename(newShaderFilename);
            doc[reference.PathKey] = path.generic_string();
            // The GUID was derived from the shader's old path: kept, it would name
            // whatever file next claims that path. The path carries the reference;
            // the next save refills the GUID from it.
            doc.erase(reference.GuidKey);
            rewritten = true;
        }
        return rewritten;
    });
}

bool RewriteMaterialName(const std::filesystem::path& materialPath,
                         const std::string& oldName,
                         const std::string& newName)
{
    return RewriteMaterialDocument(materialPath, [&](nlohmann::json& doc)
    {
        if (doc.value("materialName", std::string()) != oldName)
            return false;
        doc["materialName"] = newName;
        return true;
    });
}

AssetRenamePlan PlanAssetRename(const std::filesystem::path& currentPath, const std::string& newName)
{
    AssetRenamePlan plan;
    plan.From = currentPath;
    plan.To = currentPath.parent_path() / (newName + KeptSuffix(currentPath));

    const std::filesystem::path companion = FindPairedSurfaceShaderMaterial(currentPath);
    if (!companion.empty())
    {
        plan.CompanionFrom = companion;
        plan.CompanionTo = companion.parent_path() / (newName + companion.extension().string());
    }
    return plan;
}

std::filesystem::path ResolveRegistryAssetPath(const AssetManager* assets,
                                               const std::filesystem::path& path)
{
    if (!assets || path.empty())
        return path;
    // The registry resolves a relative path against the project root itself
    // (TryGetAssetMetadata rebases before normalizing), so every path form the
    // rename machinery accepts resolves here too.
    AssetMetadata metadata;
    if (!assets->GetRegistry().TryGetAssetMetadata(path, metadata) || metadata.Path.empty())
        return path;
    return metadata.Path;
}

} // namespace GameEngine::Editor
