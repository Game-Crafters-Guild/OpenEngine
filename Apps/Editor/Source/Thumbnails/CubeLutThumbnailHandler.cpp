#include "Thumbnails/CubeLutThumbnailHandler.h"

#include "Assets/AssetManager.h"
#include "Engine/Rendering/PostProcessSettings.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/TextureService.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Passes/HdrColorFxPass.h"
#include "Rendering/Passes/TonemapPass.h"
#include "Rendering/Utils/CubeLutColorDomain.h"
#include "Rendering/Utils/TextureUploadHelpers.h"
#include "UI/UIManager.h"
#include "UI/UITextureRegistry.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace GameEngine
{

namespace
{
using Rendering::Passes::CubeLutPreviewSource;

constexpr const char* kEnginePrefix = "engine:";

// Asset thumbnails: icon-sized requests fill a square slot; larger ones (the Asset View) are a
// wide sheet, four times as wide as tall.
constexpr int kWideThumbnailMinSize = 256;
constexpr uint32_t kWideThumbnailAspect = 4;
constexpr uint32_t kMinExtent = 8;
constexpr uint32_t kMaxExtent = 2048;

// The sheet's rows, in ninths of its height: two bands per source (the look alone, then with the
// LUT), each two ninths tall, and a one-ninth black gap between the ramp pair and the probe pair.
constexpr uint32_t kSheetBandNinths = 2;

// Components::CubeLutStage::Grade.
constexpr uint32_t kStageGrade = 1;

// CubeLutGpuBindingState::Flags: the table has a 1D shaper, whose range is the table's input range.
constexpr int32_t kCubeLutHasShaper = 2;

// The sheet's dark rows between the look alone and the look with the LUT, so the two halves of a
// pair read as two bands: an eighteenth of a band from each half (at least one row), so the seam stays
// about 2 px wide where the Asset View draws the sheet smaller than its texture; none where a band is
// too short to give rows up.
constexpr uint32_t kSheetSeparatorBandFraction = 18;
constexpr uint32_t kSheetSeparatorMinBandRows = 8;

uint32_t ClampExtent(uint32_t v)
{
    return std::clamp<uint32_t>(v, kMinExtent, kMaxExtent);
}

constexpr const char* kBandDebugName = "Editor.CubeLutStrip.Band";
} // namespace

// The black source fills the sheet's gap between the ramp pair and the probe pair.
struct CubeLutThumbnailHandler::FrameSources
{
    Rendering::RenderGraph::RGTexture Ramp;
    Rendering::RenderGraph::RGTexture Probes;
    Rendering::RenderGraph::RGTexture Black;
};

CubeLutThumbnailHandler::CubeLutThumbnailHandler(AssetManager* assetManager,
                                                 Engine::Renderer::RenderServices* renderServices)
    : m_AssetManager(assetManager)
    , m_RenderServices(renderServices)
{
    // A reload (a file edit, or a change of the asset's input encoding) re-renders the LUT's
    // previews in place. AssetReloaded arrives on the main thread, before the next frame's
    // BeginWorldDrawFrame drops the LUT's uploaded tables, so the re-render reads the new ones.
    if (m_AssetManager)
        m_ReloadInvalidator = AssetReloadInvalidator(
            m_AssetManager->GetEventDispatcher(), AssetType::CubeLut, [this](const GUID& lut) { OnLutReloaded(lut); },
            AssetReloadInvalidator::EventSet::ReloadedOnly);
}

CubeLutThumbnailHandler::~CubeLutThumbnailHandler()
{
    m_ReloadInvalidator.Reset();
    // Handler-owned textures die with the handler (the device defers the destroy behind the
    // graphics timeline).
    ReleaseAll();
    Rendering::IDevice* device = m_RenderServices ? m_RenderServices->GetDevice() : nullptr;
    for (Rendering::TextureHandle* source : {&m_Ramp, &m_Probes, &m_Black})
        if (device && source->IsValid())
            device->DestroyTexture(*source);
}

std::string CubeLutThumbnailHandler::GetOrRequest(const std::filesystem::path& assetPath,
                                                  int desiredSize,
                                                  std::function<void(const std::string& relPath)> onReady,
                                                  bool /*staticModelListThumbnail*/)
{
    if (!m_AssetManager)
        return {};
    const GUID guid = m_AssetManager->ResolveAssetGuid(m_AssetManager->ResolveAssetPath(assetPath));
    if (guid.IsNull())
        return {};
    CubeLutStripRequest request{};
    request.Lut = guid;
    request.Content = CubeLutStripContent::Sheet;
    request.Width = ClampExtent(static_cast<uint32_t>(std::max(desiredSize, 1)));
    request.Height = desiredSize >= kWideThumbnailMinSize ? ClampExtent(request.Width / kWideThumbnailAspect)
                                                          : request.Width;
    const std::string name = kEnginePrefix + MakeCubeLutStripKey(request);
    if (onReady)
        onReady(name);
    return name;
}

void CubeLutThumbnailHandler::EnsureEngineThumbnailRequested(uint64_t windowId, const std::string& engineName)
{
    // A name that is not a strip's is another handler's: open nothing for it.
    if (!ParseCubeLutStripKey(engineName))
        return;
    m_Windows[windowId].Open(engineName);
}

bool CubeLutThumbnailHandler::IsEngineThumbnailReadyForWindow(uint64_t windowId, const std::string& engineName) const
{
    const auto win = m_Windows.find(windowId);
    const auto request = ParseCubeLutStripKey(engineName);
    if (win == m_Windows.end() || !request)
        return false;
    const auto band = win->second.Live().find(MakeCubeLutStripKey(*request));
    return band != win->second.Live().end() && band->second.Rendered;
}

void CubeLutThumbnailHandler::OnLutReloaded(const GUID& lut)
{
    for (auto& [windowId, bands] : m_Windows)
        (void)bands.MarkLutChanged(lut);
}

Rendering::TextureHandle CubeLutThumbnailHandler::CreateSourceTexture(uint32_t width, const float* texels,
                                                                      const char* name)
{
    Rendering::IDevice* device = m_RenderServices ? m_RenderServices->GetDevice() : nullptr;
    if (!device)
        return {};
    Rendering::TextureDesc desc{};
    desc.width = width;
    desc.height = 1;
    desc.depth = 1;
    desc.mipLevels = 1;
    desc.arrayLayers = 1;
    desc.format = static_cast<uint32_t>(Rendering::TextureFormat::R32G32B32A32_FLOAT);
    desc.usage = static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource) |
                 static_cast<uint32_t>(Rendering::TextureUsage::TransferDst);
    desc.persistent = true;
    desc.debugName = name;
    const Rendering::TextureHandle texture = device->CreateTexture(desc);
    if (texture.IsValid())
        Rendering::UploadTexture2D(device, texture, texels, width, 1, width * 4u * sizeof(float),
                                   "Editor.CubeLutStrip.SourceStaging");
    return texture;
}

bool CubeLutThumbnailHandler::EnsureSources()
{
    if (!m_Ramp.IsValid())
    {
        const std::vector<float> ramp = Rendering::Passes::MakeCubeLutPreviewSourceTexels(CubeLutPreviewSource::Ramp);
        m_Ramp = CreateSourceTexture(Rendering::Passes::CubeLutPreviewSourceWidth(CubeLutPreviewSource::Ramp),
                                     ramp.data(), "Editor.CubeLutStrip.Ramp");
    }
    if (!m_Probes.IsValid())
    {
        const std::vector<float> probes =
            Rendering::Passes::MakeCubeLutPreviewSourceTexels(CubeLutPreviewSource::Probes);
        m_Probes = CreateSourceTexture(Rendering::Passes::CubeLutPreviewSourceWidth(CubeLutPreviewSource::Probes),
                                       probes.data(), "Editor.CubeLutStrip.Probes");
    }
    if (!m_Black.IsValid())
    {
        const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        m_Black = CreateSourceTexture(1, black, "Editor.CubeLutStrip.Black");
    }
    return m_Ramp.IsValid() && m_Probes.IsValid() && m_Black.IsValid();
}

Rendering::TextureHandle CubeLutThumbnailHandler::CreateBandTexture(const CubeLutStripRequest& request)
{
    Rendering::IDevice* device = m_RenderServices ? m_RenderServices->GetDevice() : nullptr;
    if (!device)
        return {};
    Rendering::TextureDesc desc{};
    desc.width = ClampExtent(request.Width);
    desc.height = ClampExtent(request.Height);
    desc.depth = 1;
    desc.mipLevels = 1;
    desc.arrayLayers = 1;
    desc.format = static_cast<uint32_t>(Rendering::TextureFormat::R16G16B16A16_FLOAT);
    desc.usage = static_cast<uint32_t>(Rendering::TextureUsage::RenderTarget) |
                 static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource);
    desc.persistent = true;
    desc.debugName = kBandDebugName;
    return device->CreateTexture(desc);
}

bool CubeLutThumbnailHandler::DeclareRows(Rendering::RenderGraph::RGFrame& frame,
                                          Rendering::RenderGraph::RGTexture source,
                                          Rendering::RenderGraph::RGTexture target, const CubeLutStripRequest& request,
                                          bool withLut, uint32_t rowBegin, uint32_t rowEnd)
{
    Rendering::Passes::TonemapParams tonemap{};
    tonemap.look = request.Look;
    tonemap.targetRowBegin = rowBegin;
    tonemap.targetRowEnd = rowEnd;
    if (!withLut)
        return Rendering::Passes::AddTonemapPassRG(frame, source, target, tonemap, "Editor.CubeLutStrip.Look").IsValid();

    // The LUT's tables through the same lookup the volume's nodes use.
    Engine::Renderer::PostProcessSettings lutSettings{};
    lutSettings.LutIntensity = 1.0f;
    const auto& words = request.Lut.GetData();
    std::memcpy(lutSettings.LutAssetGuidWords, words.data(), sizeof(lutSettings.LutAssetGuidWords));
    Rendering::CubeLutGpuBindingState lut{};
    (void)m_RenderServices->Textures().TryGetCubeLutGpuBinding(lutSettings, lut);
    const float intensity = std::clamp(request.Intensity, 0.0f, 1.0f);

    // An asset's sheet shows the LUT at the stage it is made for: Finish for a display LUT, Grade
    // for a log or scene LUT. An inspector's band shows the effect's stage.
    const bool atGrade = request.Content == CubeLutStripContent::Sheet
                           ? !Rendering::CubeLutExpectsDisplayInput(
                                 static_cast<Rendering::CubeLutInputEncoding>(lut.InputEncoding),
                                 (lut.Flags & kCubeLutHasShaper) != 0 ? lut.In1DMax : lut.In3DMax)
                           : request.Stage == kStageGrade;
    if (!atGrade)
    {
        tonemap.lut = lut;
        tonemap.lutIntensity = intensity;
        return Rendering::Passes::AddTonemapPassRG(frame, source, target, tonemap, "Editor.CubeLutStrip.Finish").IsValid();
    }

    // Grade: the LUT on the scene-linear source, then the look.
    const Rendering::RenderGraph::RGResourceDesc sourceDesc = frame.Graph().ResourceDesc(source.Id);
    Rendering::TextureDesc gradedDesc{};
    gradedDesc.width = std::max(sourceDesc.Width, 1u);
    gradedDesc.height = std::max(sourceDesc.Height, 1u);
    gradedDesc.depth = 1;
    gradedDesc.mipLevels = 1;
    gradedDesc.arrayLayers = 1;
    gradedDesc.sampleCount = 1;
    gradedDesc.format = static_cast<uint32_t>(Rendering::TextureFormat::R16G16B16A16_FLOAT);
    gradedDesc.usage = static_cast<uint32_t>(Rendering::TextureUsage::RenderTarget) |
                       static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource);
    gradedDesc.debugName = "Editor.CubeLutStrip.Graded";
    const Rendering::RenderGraph::RGTexture graded = frame.CreateTexture("Editor.CubeLutStrip.Graded", gradedDesc);
    Rendering::Passes::HdrColorFxParams grade{};
    grade.lut = lut;
    grade.lutIntensity = intensity;
    return graded.IsValid() &&
           Rendering::Passes::AddHdrColorFxPassRG(frame, source, graded, grade, "Editor.CubeLutStrip.Grade").IsValid() &&
           Rendering::Passes::AddTonemapPassRG(frame, graded, target, tonemap, "Editor.CubeLutStrip.Look").IsValid();
}

bool CubeLutThumbnailHandler::DeclareSheetPair(Rendering::RenderGraph::RGFrame& frame, const FrameSources& sources,
                                               Rendering::RenderGraph::RGTexture source,
                                               Rendering::RenderGraph::RGTexture target,
                                               const CubeLutStripRequest& request, uint32_t rowBegin, uint32_t bandRows)
{
    // Half the separator comes out of each band of the pair.
    const uint32_t half =
        bandRows >= kSheetSeparatorMinBandRows ? std::max(1u, bandRows / kSheetSeparatorBandFraction) : 0u;
    const uint32_t middle = rowBegin + bandRows;
    return DeclareRows(frame, source, target, request, false, rowBegin, middle - half) &&
           (half == 0 || DeclareRows(frame, sources.Black, target, request, false, middle - half, middle + half)) &&
           DeclareRows(frame, source, target, request, true, middle + half, middle + bandRows);
}

bool CubeLutThumbnailHandler::RenderBand(Rendering::RenderGraph::RGFrame& frame, const FrameSources& sources,
                                         const CubeLutStripBands::Band& band)
{
    const CubeLutStripRequest& request = band.Request;
    const Rendering::RenderGraph::RGTexture target = frame.ImportExternalTexture(
        kBandDebugName, band.Texture, Rendering::ResourceState::Undefined, Rendering::TextureFormat::R16G16B16A16_FLOAT);
    const uint32_t height = ClampExtent(request.Height);

    bool declared = false;
    switch (request.Content)
    {
    case CubeLutStripContent::LookAlone:
    case CubeLutStripContent::WithLut:
    {
        const Rendering::RenderGraph::RGTexture source =
            request.Source == CubeLutPreviewSource::Probes ? sources.Probes : sources.Ramp;
        declared = DeclareRows(frame, source, target, request, request.Content == CubeLutStripContent::WithLut, 0, 0);
        break;
    }
    case CubeLutStripContent::Sheet:
    {
        const uint32_t bandRows = std::max(1u, height * kSheetBandNinths / 9u);
        const uint32_t probesBegin = height - 2u * bandRows;
        declared = DeclareSheetPair(frame, sources, sources.Ramp, target, request, 0, bandRows) &&
                   DeclareRows(frame, sources.Black, target, request, false, 2u * bandRows, probesBegin) &&
                   DeclareSheetPair(frame, sources, sources.Probes, target, request, probesBegin, bandRows);
        break;
    }
    }
    if (declared)
        frame.MarkOutput(target, Rendering::RenderGraph::RGImageLayout::ShaderReadOnly);
    return declared;
}

void CubeLutThumbnailHandler::TickRenderRG(uint64_t windowId, Rendering::RenderGraph::RGFrame& frame)
{
    auto win = m_Windows.find(windowId);
    if (win == m_Windows.end() || !m_RenderServices)
        return;

    bool anyPending = false;
    for (const auto& [key, band] : win->second.Live())
        anyPending = anyPending || !band.Rendered;
    if (!anyPending || !EnsureSources())
        return;

    FrameSources sources{};
    sources.Ramp = frame.ImportExternalTexture("Editor.CubeLutStrip.Ramp", m_Ramp, Rendering::ResourceState::ShaderResource,
                                               Rendering::TextureFormat::R32G32B32A32_FLOAT);
    sources.Probes = frame.ImportExternalTexture("Editor.CubeLutStrip.Probes", m_Probes,
                                                 Rendering::ResourceState::ShaderResource,
                                                 Rendering::TextureFormat::R32G32B32A32_FLOAT);
    sources.Black = frame.ImportExternalTexture("Editor.CubeLutStrip.Black", m_Black,
                                                Rendering::ResourceState::ShaderResource,
                                                Rendering::TextureFormat::R32G32B32A32_FLOAT);
    for (auto& [key, band] : win->second.Live())
    {
        if (band.Rendered)
            continue;
        if (!band.Texture.IsValid())
            band.Texture = CreateBandTexture(band.Request);
        if (band.Texture.IsValid() && RenderBand(frame, sources, band))
            band.Rendered = true;
    }
}

bool CubeLutThumbnailHandler::RegisterReadyThumbnailsRG(uint64_t windowId, UIManager* ui,
                                                       Rendering::RenderGraph::RGFrame* pureFrame)
{
    auto win = m_Windows.find(windowId);
    if (!ui || win == m_Windows.end())
        return false;

    // Textures an owner's newer name replaced: off the UI first, then destroyed.
    CubeLutStripBands::Retired retired = win->second.TakeRetired();
    bool changed = !retired.empty();
    DestroyTextures(retired, ui);

    for (auto& [key, band] : win->second.Live())
    {
        if (!band.Rendered || !band.Texture.IsValid())
            continue;
        const uint32_t width = ClampExtent(band.Request.Width);
        const uint32_t height = ClampExtent(band.Request.Height);
        if (!band.UiBound)
            changed = true;
        // The tonemap emits display-referred linear [0, 1]: the same producer and space as the
        // model thumbnails.
        if (pureFrame)
        {
            ui->SetExternalTextureRG(key, width, height, UI::UITextureSpace::DisplayLinearSdr(),
                                     Rendering::TextureFormat::R16G16B16A16_FLOAT);
            const Rendering::RenderGraph::RGTexture tex = pureFrame->ImportExternalTexture(
                kBandDebugName, band.Texture, Rendering::ResourceState::ShaderResource,
                Rendering::TextureFormat::R16G16B16A16_FLOAT);
            if (tex.IsValid())
                ui->PublishExternalTextureRG(key, *pureFrame, tex);
        }
        else
        {
            ui->SetExternalTexture(key, band.Texture, width, height, UI::UITextureSpace::DisplayLinearSdr());
        }
        band.UiBound = true;
    }
    return changed;
}

void CubeLutThumbnailHandler::ResetForProjectSwitch()
{
    ReleaseAll();
}

void CubeLutThumbnailHandler::DestroyTextures(CubeLutStripBands::Retired& retired, UIManager* ui)
{
    Rendering::IDevice* device = m_RenderServices ? m_RenderServices->GetDevice() : nullptr;
    for (auto& [key, band] : retired)
    {
        if (ui && band.UiBound)
            ui->RemoveExternalTexture(key);
        if (device && band.Texture.IsValid())
            device->DestroyTexture(band.Texture);
    }
    retired.clear();
}

void CubeLutThumbnailHandler::ReleaseAll()
{
    for (auto& [windowId, bands] : m_Windows)
    {
        CubeLutStripBands::Retired all = bands.TakeAll();
        DestroyTextures(all, nullptr);
    }
    m_Windows.clear();
}

} // namespace GameEngine
