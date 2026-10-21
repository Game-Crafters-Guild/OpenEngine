#include "Editor/Settings/RenderLayersSettingsPage.h"

#include "Core/Engine.h"
#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/SettingsStore.h"
#include "Engine/Rendering/RenderLayerNames.h"
#include "Inspectors/RenderLayerField.h"
#include "Logger/Logger.h"

#include <string>
#include <utility>

namespace GameEngine::Editor
{
namespace
{
const std::filesystem::path& WorkspaceRoot()
{
    return EngineCore::GetInstance().GetWorkspaceRoot();
}

// Writes the names into the project file, keeping every other "rendering" key.
bool SaveRenderLayerNames(const std::filesystem::path& workspaceRoot, const Rendering::RenderLayerNames& names)
{
    if (workspaceRoot.empty())
        return false;
    SettingsStore store = OpenProjectSettings(workspaceRoot);
    std::string error;
    (void)store.Load(&error);
    auto& root = store.Json();
    if (!root.is_object())
        root = nlohmann::json::object();
    auto& rendering = root[Rendering::RenderLayerNames::kRenderingKey];
    if (!rendering.is_object())
        rendering = nlohmann::json::object();
    names.WriteTo(rendering);
    if (!store.Save(&error))
    {
        Logger::Log::Error("RenderLayers: cannot save the layer names: {}", error);
        return false;
    }
    return true;
}

// Names `layer` in the project file and in the running editor, and gives every layer picker the new names.
void CommitLayerName(uint32 layer, const std::string& name)
{
    Rendering::RenderLayerNames names = Rendering::RenderLayerNames::Load(WorkspaceRoot());
    if (names.Authored(layer) == name || !names.Set(layer, name).empty())
        return;
    if (!SaveRenderLayerNames(WorkspaceRoot(), names))
        Logger::Log::Warning("RenderLayers: layer {} is named '{}' for this session only: the project has no "
                             "writable settings file",
                             layer, name);
    Rendering::SetRenderLayerNames(names);
    RefreshRenderLayerFields();
}

SettingsFieldDescriptor MakeLayerRow(uint32 layer)
{
    SettingsFieldDescriptor row;
    row.Label = "Layer " + std::to_string(layer);
    row.Tooltip = "The name of render layer " + std::to_string(layer) +
                  ". Pickers and scripts use it; scenes store the number. Clear it to use the default name.";
    row.SearchKeywords = "render layer name rename";
    SettingsFieldDescriptor::StringField text;
    text.Placeholder = Rendering::RenderLayerNames::FallbackName(layer);
    text.Get = [layer]() { return std::string(Rendering::RenderLayerNames::Load(WorkspaceRoot()).Authored(layer)); };
    text.Set = [layer](const std::string& name) { CommitLayerName(layer, name); };
    text.Validate = [layer](const std::string& name)
    { return Rendering::RenderLayerNames::Load(WorkspaceRoot()).Refusal(layer, name); };
    row.Control = std::move(text);
    return row;
}
} // namespace

void RegisterRenderLayersSettingsCategory()
{
    SettingsCategoryDescriptor page;
    page.CategoryId = "renderLayers";
    page.Title = "Render Layers";
    page.Group = SettingsCategoryGroup::ProjectSettings;
    page.SearchKeywords = "render layers layer names mask culling camera visible";
    page.Description =
        "The names of the 32 render layers. Scenes store layer numbers, so a rename reaches every picker and "
        "every script lookup and changes no scene. This page cannot show where a layer is used: turn on Show "
        "Render Layer in the Hierarchy's options to see each object's layers.";
    for (uint32 layer = 0; layer < Rendering::RenderLayerMask::kCount; ++layer)
        page.Fields.push_back(MakeLayerRow(layer));
    EditorSettingsRegistry::Get().RegisterCategory(std::move(page));
}

} // namespace GameEngine::Editor
