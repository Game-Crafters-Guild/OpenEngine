#pragma once

#include "Rendering/RenderLayerMask.h"
#include "Types/Types.h"

#include <array>
#include <filesystem>
#include <string>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

namespace GameEngine::Rendering
{

/// The project's render layer names: the table behind Rendering::Script::Find and NameOf
/// (Rendering/RenderLayersScriptApi.h), and the sole owner of its serialized shape. For the hosts only
/// (the editor's settings page, the Player's startup, BuildPipeline); a game reads the names through
/// RenderLayersScriptApi.h. Two transports carry the same key:
///
///   authored  <ProjectRoot>/.Editor/ProjectSettings.json -> "rendering": { "renderLayerNames": [...] }
///   shipped   <exe dir>/game.config                      -> "rendering": { "renderLayerNames": [...] }
///
///   "renderLayerNames": ["Default", "Props", "", "Foliage"]     entry i names layer i; "" is unnamed
///
/// Each name is at most 63 bytes of UTF-8, stored inline, so the table is about 2 KB and never allocates.
struct RenderLayerNames
{
    static constexpr uint32 kMaxNameBytes = 63;
    static constexpr const char* kRenderingKey = "rendering";
    static constexpr const char* kNamesKey = "renderLayerNames";

    /// The authored names, one per layer, each ending in a zero byte. An empty name means the layer is
    /// unnamed.
    std::array<std::array<char, kMaxNameBytes + 1>, RenderLayerMask::kCount> Names{};

    /// The name an unnamed `layer` shows: "Default" for layer 0, "Layer N" for the others.
    static std::string FallbackName(uint32 layer);

    /// The authored name of `layer`, empty when it is unnamed.
    std::string_view Authored(uint32 layer) const;

    /// The authored name of `layer`, else its fallback name.
    std::string EffectiveName(uint32 layer) const;

    /// Why `name` cannot be the name of `layer`, or an empty string when it can. Refused: another layer's
    /// name or default name (so clearing a name never leaves two layers with one), a name over 63 bytes of
    /// UTF-8, and a name of only spaces. An empty name is accepted and makes the layer unnamed.
    std::string Refusal(uint32 layer, std::string_view name) const;

    /// Names `layer` when Refusal accepts the name, and returns Refusal's message.
    std::string Set(uint32 layer, std::string_view name);

    /// Reads the "renderLayerNames" array of a "rendering" object. An entry Refusal refuses is left
    /// unnamed, with a warning that names the entry.
    static RenderLayerNames ReadFrom(const nlohmann::json& rendering);

    /// Writes the "renderLayerNames" key into a "rendering" object and leaves every other key as it is.
    /// Unnamed layers after the last named one are left out; with no name at all the key is removed.
    void WriteTo(nlohmann::json& rendering) const;

    /// Reads <workspaceRoot>/.Editor/ProjectSettings.json: the editor's project open and the build's cook.
    static RenderLayerNames Load(const std::filesystem::path& workspaceRoot);
};

/// Makes `names` the table Script::Find and Script::NameOf answer from. Main thread, between frames:
/// the editor's project open and settings page, and the Player's startup.
void SetRenderLayerNames(const RenderLayerNames& names);

} // namespace GameEngine::Rendering
