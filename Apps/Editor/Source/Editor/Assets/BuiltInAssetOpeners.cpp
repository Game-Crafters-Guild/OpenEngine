#include "Editor/Assets/BuiltInAssetOpeners.h"

#include "Editor/Registries/AssetOpenerRegistry.h"
#include "Graph/GraphKindChrome.h"

#include "AssetCore/AssetTypes.h"
#include "Types/PathUtils.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace GameEngine::Editor
{

namespace
{

using Actions = std::shared_ptr<const BuiltInAssetOpenActions>;

bool IsAnimationFile(std::string_view extension)
{
    return extension == ".anim" || extension == ".animation" || extension == ".fbx" || extension == ".gltf" ||
           extension == ".glb";
}

// A hand-written shader is text; only a graph-generated one, and only when the user wants it so, opens in the
// Material Graph.
bool OpenGraphFile(const Actions& actions, const std::filesystem::path& path)
{
    if (LowerCaseExtension(path) == ".glsl" && !actions->ReadPreferences(path).GlslOpensInMaterialGraph)
        return actions->OpenInScriptEditor(path);
    return actions->OpenInNodeGraph(path);
}

bool OpenCSharp(const Actions& actions, const std::filesystem::path& path)
{
    if (!actions->ReadPreferences(path).OpenCSharpInScriptEditor)
        return actions->OpenInExternalScriptEditor(path);
    // The external editor is the second try when the Script Editor cannot take the file.
    return actions->OpenInScriptEditor(path) || actions->OpenInExternalScriptEditor(path);
}

bool OpenNativeSource(const Actions& actions, const std::filesystem::path& path)
{
    return actions->ReadPreferences(path).OpenNativeSourceInScriptEditor ? actions->OpenInScriptEditor(path)
                                                                         : actions->OpenInIde(path);
}

} // namespace

void RegisterBuiltInAssetOpeners(AssetOpenerRegistry& registry, BuiltInAssetOpenActions actions)
{
    const Actions shared = std::make_shared<const BuiltInAssetOpenActions>(std::move(actions));
    // No two built-in openers take the same extension.
    registry.Register({"scene", [](std::string_view ext) { return ext == ".scene"; },
                       [shared](const std::filesystem::path& path) { return shared->OpenScene(path); }});
    registry.Register({"csharp", [](std::string_view ext) { return ext == ".cs"; },
                       [shared](const std::filesystem::path& path) { return OpenCSharp(shared, path); }});
    registry.Register({"nativeSource",
                       [](std::string_view ext)
                       { return GetAssetTypeFromExtension(std::string(ext)) == AssetType::NativeSource; },
                       [shared](const std::filesystem::path& path) { return OpenNativeSource(shared, path); }});
    registry.Register({"material",
                       [](std::string_view ext)
                       { return GetAssetTypeFromExtension(std::string(ext)) == AssetType::Material; },
                       [shared](const std::filesystem::path& path) { return shared->ShowInInspector(path); }});
    registry.Register({"graph",
                       [](std::string_view ext) { return ExtensionOpensInGraphPanel(ext); },
                       [shared](const std::filesystem::path& path) { return OpenGraphFile(shared, path); }});
    registry.Register({"timeline", [](std::string_view ext) { return ext == ".timeline"; },
                       [shared](const std::filesystem::path& path)
                       { return shared->OpenInAnimationWindow(path, AnimationAssetWindow::Timeline); }});
    registry.Register({"clipSet", [](std::string_view ext) { return ext == ".clipset"; },
                       [shared](const std::filesystem::path& path)
                       { return shared->OpenInAnimationWindow(path, AnimationAssetWindow::ClipEditor); }});
    registry.Register({"animation",
                       [](std::string_view ext) { return IsAnimationFile(ext); },
                       [shared](const std::filesystem::path& path)
                       { return shared->OpenInAnimationWindow(path, AnimationAssetWindow::Animation); }});
}

} // namespace GameEngine::Editor
