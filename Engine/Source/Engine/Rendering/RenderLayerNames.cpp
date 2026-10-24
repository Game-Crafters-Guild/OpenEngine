#include "Engine/Rendering/RenderLayerNames.h"

#include "Logger/Logger.h"
#include "Rendering/RenderLayersScriptApi.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <fstream>
#include <mutex>

#include <nlohmann/json.hpp>

namespace GameEngine::Rendering
{
namespace
{
// The editor's per-project settings file. Read-only from here; the settings page is its writer.
constexpr const char* kEditorDirName = ".Editor";
constexpr const char* kProjectSettingsFileName = "ProjectSettings.json";

// The table Script::Find and Script::NameOf answer from. Written between frames on the main thread;
// the lock keeps a read at a system's start on another thread whole.
std::mutex g_TableMutex;
RenderLayerNames g_Table;

bool IsOnlySpaces(std::string_view name)
{
    return !name.empty() &&
           std::all_of(name.begin(), name.end(), [](char c) { return c == ' ' || c == '\t'; });
}
} // namespace

std::string RenderLayerNames::FallbackName(uint32 layer)
{
    return layer == 0u ? std::string("Default") : "Layer " + std::to_string(layer);
}

std::string_view RenderLayerNames::Authored(uint32 layer) const
{
    if (layer >= RenderLayerMask::kCount)
        return {};
    return std::string_view(Names[layer].data());
}

std::string RenderLayerNames::EffectiveName(uint32 layer) const
{
    const std::string_view authored = Authored(layer);
    return authored.empty() ? FallbackName(layer) : std::string(authored);
}

std::string RenderLayerNames::Refusal(uint32 layer, std::string_view name) const
{
    if (layer >= RenderLayerMask::kCount)
        return "A render layer is 0 to 31.";
    if (name.empty())
        return {};
    if (name.size() > kMaxNameBytes)
        return "'" + std::string(name) + "' is too long: a layer name is at most 63 bytes of UTF-8.";
    if (IsOnlySpaces(name))
        return "A layer name cannot be only spaces. Clear the name to use the default one.";
    for (uint32 other = 0; other < RenderLayerMask::kCount; ++other)
    {
        if (other == layer)
            continue;
        // Another layer's default name is never taken, even while that layer has a name of its own: clearing
        // that name would bring the default back and leave two layers with one name.
        if (FallbackName(other) == name)
            return "'" + std::string(name) + "' is the default name of layer " + std::to_string(other) + ".";
        if (EffectiveName(other) == name)
            return "'" + std::string(name) + "' is already layer " + std::to_string(other) + ".";
    }
    return {};
}

std::string RenderLayerNames::Set(uint32 layer, std::string_view name)
{
    std::string refusal = Refusal(layer, name);
    if (!refusal.empty())
        return refusal;
    Names[layer].fill('\0');
    std::memcpy(Names[layer].data(), name.data(), name.size());
    return {};
}

RenderLayerNames RenderLayerNames::ReadFrom(const nlohmann::json& rendering)
{
    RenderLayerNames names;
    const auto it = rendering.find(kNamesKey);
    if (it == rendering.end() || !it->is_array())
        return names;
    const uint32 count = std::min<uint32>(static_cast<uint32>(it->size()), RenderLayerMask::kCount);
    for (uint32 layer = 0; layer < count; ++layer)
    {
        const nlohmann::json& entry = (*it)[layer];
        if (!entry.is_string())
            continue;
        const std::string refusal = names.Set(layer, entry.get_ref<const std::string&>());
        if (!refusal.empty())
            Logger::Log::Warning("RenderLayerNames: rendering.{}[{}] is left unnamed. {}", kNamesKey, layer,
                                 refusal);
    }
    return names;
}

void RenderLayerNames::WriteTo(nlohmann::json& rendering) const
{
    uint32 count = 0;
    for (uint32 layer = 0; layer < RenderLayerMask::kCount; ++layer)
    {
        if (!Authored(layer).empty())
            count = layer + 1u;
    }
    if (count == 0u)
    {
        rendering.erase(kNamesKey);
        return;
    }
    nlohmann::json array = nlohmann::json::array();
    for (uint32 layer = 0; layer < count; ++layer)
        array.push_back(std::string(Authored(layer)));
    rendering[kNamesKey] = std::move(array);
}

RenderLayerNames RenderLayerNames::Load(const std::filesystem::path& workspaceRoot)
{
    if (workspaceRoot.empty())
        return {};
    const std::filesystem::path settingsPath =
        (workspaceRoot / kEditorDirName / kProjectSettingsFileName).lexically_normal();
    std::ifstream file(settingsPath);
    if (!file.is_open())
        return {};
    const nlohmann::json root = nlohmann::json::parse(file, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded() || !root.is_object())
    {
        Logger::Log::Warning("RenderLayerNames: '{}' is not readable JSON; every layer keeps its default name",
                             settingsPath.generic_string());
        return {};
    }
    const auto rendering = root.find(kRenderingKey);
    if (rendering == root.end() || !rendering->is_object())
        return {};
    return ReadFrom(*rendering);
}

void SetRenderLayerNames(const RenderLayerNames& names)
{
    std::lock_guard<std::mutex> lock(g_TableMutex);
    g_Table = names;
}

RenderLayerMask Script::Find(std::string_view name)
{
    std::lock_guard<std::mutex> lock(g_TableMutex);
    for (uint32 layer = 0; layer < RenderLayerMask::kCount; ++layer)
    {
        if (g_Table.EffectiveName(layer) == name)
            return RenderLayerMask::FromLayer(layer);
    }
    return RenderLayerMask::None();
}

std::string Script::NameOf(RenderLayerMask layer)
{
    if (std::popcount(layer.Bits) != 1)
        return {};
    std::lock_guard<std::mutex> lock(g_TableMutex);
    return g_Table.EffectiveName(static_cast<uint32>(std::countr_zero(layer.Bits)));
}

} // namespace GameEngine::Rendering
