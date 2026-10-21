#include "Assets/TexturePreparingStatusItem.h"

#include "Assets/AssetManager.h"
#include "Assets/TextureCookActivity.h"
#include "Core/Engine.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/TextureService.h"

#include <algorithm>
#include <utility>

namespace GameEngine::Editor
{
namespace
{
TexturePreparingCounts ReadEngineCounts()
{
    TexturePreparingCounts counts;
    EngineCore& engine = EngineCore::GetInstance();
    if (AssetManager* assets = engine.TryGetAssetManager())
        counts.Cooking = assets->GetTextureCookActivity().CookingCount();
    if (Engine::Renderer::RenderServices* renderServices = engine.GetRenderServices())
        counts.AwaitedByMaterials = renderServices->Textures().PendingMaterialTextureCount();
    return counts;
}
} // namespace

StatusBarItemDisplay TexturePreparingStatusDisplay(const TexturePreparingCounts& counts)
{
    const size_t preparing = std::max(counts.Cooking, counts.AwaitedByMaterials);
    if (preparing == 0)
        return {};
    const std::string textures =
        preparing == 1 ? std::string("1 texture") : std::to_string(preparing) + " textures";
    return {"Preparing " + textures, textures + " still preparing; clears when done"};
}

StatusBarItem MakeTexturePreparingStatusItem(std::function<TexturePreparingCounts()> counts)
{
    return StatusBarItem{"TexturesPreparing",
                         [counts = std::move(counts)]() { return TexturePreparingStatusDisplay(counts()); }};
}

void RegisterTexturePreparingStatusItem(StatusBarItemRegistry& registry)
{
    registry.Register(MakeTexturePreparingStatusItem(&ReadEngineCounts));
}

} // namespace GameEngine::Editor
