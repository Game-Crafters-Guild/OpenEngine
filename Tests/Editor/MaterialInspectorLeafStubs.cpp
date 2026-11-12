// A no-op link stub for the material inspector that the Mesh Renderer inspector embeds. EditorTests test the Mesh
// Renderer rows, not the material UI, which needs the rendering and asset dependencies of MaterialInspector.cpp.

#include "Inspectors/MaterialInspector.h"

namespace GameEngine
{

void BuildMaterialInspectorUI(UIElement*, MaterialAsset*, OpenColorPickerWindowFn,
                              std::function<void(const std::filesystem::path&)>, Editor::UndoRedoService*, bool,
                              OpenAssetFn, std::function<void(const std::filesystem::path&)>)
{
}

} // namespace GameEngine
