#pragma once

#include "Thumbnails/AssetThumbnailHandler.h"
#include "Thumbnails/CubeLutStripBands.h"

#include "AssetCore/AssetReloadInvalidator.h"
#include "Rendering/Core/Device.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace GameEngine
{

class AssetManager;

namespace Rendering::RenderGraph
{
struct RGTexture;
}

namespace Engine::Renderer
{
class RenderServices;
}

/// Renders Cube LUT previews: a scene-linear source (the neutral exposure ramp, mid-gray times
/// 2^-4 to 2^+4, or the four saturated probes) through the real passes the frame uses, as engine
/// textures the UI samples. At the Finish stage the source runs through the Tonemap pass with the
/// LUT; at the Grade stage through the HDRColorFx pass with the LUT, then the Tonemap pass. A
/// preview is named by what it shows (MakeCubeLutStripKey): the inspector builds the names of its
/// bands itself, and the asset thumbnail (GetOrRequest) is the sheet of both sources at the Neutral
/// look, Finish, full intensity. A texture renders in a window when that window's UI first asks for
/// its name, and again after its LUT reloads (a file edit, or a change of the asset's settings).
class CubeLutThumbnailHandler final : public AssetThumbnailHandler
{
  public:
    CubeLutThumbnailHandler(AssetManager* assetManager, Engine::Renderer::RenderServices* renderServices);
    ~CubeLutThumbnailHandler() override;

    std::string GetOrRequest(const std::filesystem::path& assetPath,
                             int desiredSize,
                             std::function<void(const std::string& relPath)> onReady,
                             bool staticModelListThumbnail = false) override;
    void ResetForProjectSwitch() override;
    void TickRenderRG(uint64_t windowId, Rendering::RenderGraph::RGFrame& frame) override;
    bool RegisterReadyThumbnailsRG(uint64_t windowId, UIManager* ui,
                                   Rendering::RenderGraph::RGFrame* pureFrame) override;
    void EnsureEngineThumbnailRequested(uint64_t windowId, const std::string& engineName) override;
    bool IsEngineThumbnailReadyForWindow(uint64_t windowId, const std::string& engineName) const override;

  private:
    // The preview sources imported into a frame (defined with the handler's source).
    struct FrameSources;

    bool EnsureSources();
    Rendering::TextureHandle CreateSourceTexture(uint32_t width, const float* texels, const char* name);
    Rendering::TextureHandle CreateBandTexture(const CubeLutStripRequest& request);
    bool RenderBand(Rendering::RenderGraph::RGFrame& frame, const FrameSources& sources,
                    const CubeLutStripBands::Band& band);
    bool DeclareRows(Rendering::RenderGraph::RGFrame& frame, Rendering::RenderGraph::RGTexture source,
                     Rendering::RenderGraph::RGTexture target, const CubeLutStripRequest& request, bool withLut,
                     uint32_t rowBegin, uint32_t rowEnd);
    bool DeclareSheetPair(Rendering::RenderGraph::RGFrame& frame, const FrameSources& sources,
                          Rendering::RenderGraph::RGTexture source, Rendering::RenderGraph::RGTexture target,
                          const CubeLutStripRequest& request, uint32_t rowBegin, uint32_t bandRows);
    void OnLutReloaded(const GUID& lut);
    void DestroyTextures(CubeLutStripBands::Retired& retired, UIManager* ui);
    void ReleaseAll();

    AssetManager* m_AssetManager = nullptr;
    Engine::Renderer::RenderServices* m_RenderServices = nullptr;
    Rendering::TextureHandle m_Ramp{};
    Rendering::TextureHandle m_Probes{};
    Rendering::TextureHandle m_Black{};
    std::unordered_map<uint64_t, CubeLutStripBands> m_Windows;
    AssetReloadInvalidator m_ReloadInvalidator;
};

} // namespace GameEngine
