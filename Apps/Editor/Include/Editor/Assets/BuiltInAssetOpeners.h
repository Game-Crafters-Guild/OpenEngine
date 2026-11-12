#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>

namespace GameEngine::Editor
{

class AssetOpenerRegistry;

/// The preferences that choose where a script or a generated shader opens. Read at each open.
struct AssetOpenPreferences
{
    bool OpenCSharpInScriptEditor = true;
    bool OpenNativeSourceInScriptEditor = false;
    /// Combines the "shader graph GLSL opens in the Material Graph" setting with the file-head probe;
    /// false for a hand-written shader.
    bool GlslOpensInMaterialGraph = false;
};

/// The window an animation asset opens in.
enum class AnimationAssetWindow : uint8_t
{
    Animation,
    Timeline,
    ClipEditor,
};

/// The editor's own open actions. Each returns false when the open failed.
struct BuiltInAssetOpenActions
{
    std::function<AssetOpenPreferences(const std::filesystem::path& path)> ReadPreferences;
    std::function<bool(const std::filesystem::path& path)> OpenScene;
    std::function<bool(const std::filesystem::path& path)> OpenInScriptEditor;
    std::function<bool(const std::filesystem::path& path)> OpenInExternalScriptEditor;
    std::function<bool(const std::filesystem::path& path)> OpenInIde;
    std::function<bool(const std::filesystem::path& path)> OpenInNodeGraph;
    std::function<bool(const std::filesystem::path& path)> ShowInInspector;
    std::function<bool(const std::filesystem::path& path, AnimationAssetWindow window)> OpenInAnimationWindow;
};

/// Registers the openers of the editor's own asset kinds: scenes, C# and native source, materials,
/// graph files, timelines, clip sets and animation files.
void RegisterBuiltInAssetOpeners(AssetOpenerRegistry& registry, BuiltInAssetOpenActions actions);

} // namespace GameEngine::Editor
