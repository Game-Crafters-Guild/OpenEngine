#include "UI/Controls/SceneViewLayersField.h"

#include "Core/Engine.h"
#include "Inspectors/RenderLayerField.h"
#include "SceneView/SceneViewLayerFilter.h"
#include "UI/Registration/ElementRegistration.h"

namespace GameEngine
{
namespace
{
const std::filesystem::path& WorkspaceRoot()
{
    return EngineCore::GetInstance().GetWorkspaceRoot();
}
} // namespace

SceneViewLayersField::SceneViewLayersField()
{
    AddClass("sceneview-layers-field");
    SetTooltip("The render layers the Scene View draws. The Game View draws its camera's layers.");
    Editor::FeedRenderLayerNames(*this);
    SetNothingTooltip("The Scene View draws no layer.");
    ShowOpenProjectsFilter();
    SetOnBitsChanged([this](uint32 bits, bool on) { OnLayersChanged(bits, on); });
    m_Refreshed = Editor::RenderLayerFieldsRefreshed().Subscribe([this]() { ShowOpenProjectsFilter(); });
}

void SceneViewLayersField::ShowOpenProjectsFilter()
{
    SetValueWithoutNotify(Editor::SceneViewLayerFilter::Get().Layers(WorkspaceRoot()).Bits);
}

void SceneViewLayersField::OnLayersChanged(uint32 bits, bool on)
{
    Rendering::RenderLayerMask layers = Editor::SceneViewLayerFilter::Get().Layers(WorkspaceRoot());
    layers.Bits = on ? (layers.Bits | bits) : (layers.Bits & ~bits);
    Editor::SceneViewLayerFilter::Get().SetLayers(WorkspaceRoot(), layers);
}

} // namespace GameEngine

namespace RegisterWidgets
{
static auto s_reg_sceneViewLayersField =
    GameEngine::UIRegistration::RegisterWithFactory<GameEngine::SceneViewLayersField>(
        "SceneViewLayersField", []() { return std::make_unique<GameEngine::SceneViewLayersField>(); });
}
