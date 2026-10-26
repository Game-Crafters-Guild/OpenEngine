#include "Inspectors/CubeLutPreview.h"

#include "Inspectors/CubeLutPreviewPopover.h"
#include "Input/KeyCodes.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Mathematics/Interpolation.h"
#include "Rendering/Passes/CubeLutPreviewSources.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace GameEngine
{
namespace
{
using Rendering::Passes::CubeLutPreviewSource;

// The band textures: wider than the bands are drawn, so a wide field never stretches a texel
// across more than a pixel or two. Heights are the band classes' CSS heights (CubeLutPreview.css).
struct PreviewExtents
{
    uint32_t Width;
    uint32_t RampHeight;
    uint32_t ProbeHeight;
    const char* RootClass;
    const char* RampBandClass;
    const char* ProbeBandClass;
};

constexpr PreviewExtents kInspectorExtents{512, 12, 14, "cube-lut-preview", "cube-lut-band-ramp", "cube-lut-band-probes"};
constexpr PreviewExtents kEnlargedExtents{1024, 28, 40, "cube-lut-preview-enlarged", "cube-lut-band-ramp-enlarged",
                                          "cube-lut-band-probes-enlarged"};

const PreviewExtents& ExtentsFor(CubeLutPreviewSize size)
{
    return size == CubeLutPreviewSize::Enlarged ? kEnlargedExtents : kInspectorExtents;
}

// Binds a band to its strip texture. The resource-name helper fits the image inside the box
// (Contain), which draws a 512-texel band a few pixels tall in a narrow field; dropping that size
// leaves the band's stylesheet to stretch the image over the band's box.
void BindBandTexture(UIElement& band, const CubeLutStripRequest& request)
{
    UI::Layout::SetBackgroundResourceName(band, MakeCubeLutStripKey(request));
    band.Styles().ResetBackgroundSize();
}

// One band with its name beside it, so the band says what it is without a hover.
UIElement* AddBand(UIElement* preview, const CubeLutStripRequest& request, const char* name, const char* bandClass,
                   const char* tooltip)
{
    auto line = std::make_unique<UIElement>();
    line->AddClass("cube-lut-strip-line");
    InspectorUI::AddTextBlock(line.get(), name, "cube-lut-strip-label");
    auto band = std::make_unique<UIElement>();
    band->AddClass("cube-lut-strip-band");
    band->AddClass(bandClass);
    band->SetTooltip(tooltip);
    BindBandTexture(*band, request);
    UIElement* added = band.get();
    line->AddChild(std::move(band));
    preview->AddChild(std::move(line));
    return added;
}

// Under the ramp, aligned with it: a tick at mid-gray (0 EV), the ramp's center; in the enlarged view
// also the stops at the ramp's ends and center, in the style of the probes' stops.
void AddRampScale(UIElement* preview, CubeLutPreviewSize size)
{
    auto scale = std::make_unique<UIElement>();
    scale->AddClass("cube-lut-strip-scale");
    scale->SetTooltip("Mid-gray (0 EV); the ramp spans 4 stops either side");
    auto tick = std::make_unique<UIElement>();
    tick->AddClass("cube-lut-strip-tick");
    scale->AddChild(std::move(tick));
    preview->AddChild(std::move(scale));
    if (size != CubeLutPreviewSize::Enlarged)
        return;
    auto stops = std::make_unique<UIElement>();
    stops->AddClass("cube-lut-ramp-stops");
    InspectorUI::AddTextBlock(stops.get(), "-4", "cube-lut-ramp-stop-start");
    InspectorUI::AddTextBlock(stops.get(), "0", "cube-lut-ramp-stop-center");
    InspectorUI::AddTextBlock(stops.get(), "+4", "cube-lut-ramp-stop-end");
    preview->AddChild(std::move(stops));
}

// A stop as the preview labels it: "-2", "0", "+1".
std::string StopText(float stop)
{
    const int stops = static_cast<int>(stop);
    return stops > 0 ? "+" + std::to_string(stops) : std::to_string(stops);
}

// "a", "a and b", "a, b and c".
std::string JoinAsList(const std::vector<std::string>& items)
{
    std::string text;
    for (size_t i = 0; i < items.size(); ++i)
        text += (i == 0 ? "" : i + 1 == items.size() ? " and " : ", ") + items[i];
    return text;
}

// The probe bands' tooltip, from the shared probe table: the probes and their stops, then what the
// band runs them through.
std::string ProbesTooltip(std::string_view through)
{
    std::vector<std::string> names;
    for (const Rendering::Passes::CubeLutPreviewProbe& probe : Rendering::Passes::kCubeLutPreviewProbes)
        names.emplace_back(probe.Name);
    if (!names.empty() && !names[0].empty())
        names[0][0] = static_cast<char>(std::toupper(static_cast<unsigned char>(names[0][0])));
    std::vector<std::string> stops;
    for (const float stop : Rendering::Passes::kCubeLutPreviewProbeStops)
        stops.push_back(StopText(stop));
    return JoinAsList(names) + " at " + JoinAsList(stops) + " stops, through " + std::string(through);
}

// Under the probe bands, one name per probe block; in the enlarged view also the exposure of each
// patch, in stops.
void AddProbeNames(UIElement* preview, CubeLutPreviewSize size)
{
    if (size == CubeLutPreviewSize::Enlarged)
    {
        auto stops = std::make_unique<UIElement>();
        stops->AddClass("cube-lut-probe-names");
        for (size_t probe = 0; probe < Rendering::Passes::kCubeLutPreviewProbes.size(); ++probe)
            for (const float stop : Rendering::Passes::kCubeLutPreviewProbeStops)
                InspectorUI::AddTextBlock(stops.get(), StopText(stop), "cube-lut-probe-name");
        preview->AddChild(std::move(stops));
    }
    auto names = std::make_unique<UIElement>();
    names->AddClass("cube-lut-probe-names");
    for (const Rendering::Passes::CubeLutPreviewProbe& probe : Rendering::Passes::kCubeLutPreviewProbes)
        InspectorUI::AddTextBlock(names.get(), std::string(probe.Name), "cube-lut-probe-name");
    preview->AddChild(std::move(names));
}

// The source, content and size a band names, on top of the preview's own request.
CubeLutStripRequest BandRequest(const CubeLutStripRequest& base, CubeLutPreviewSource source,
                                CubeLutStripContent content, const PreviewExtents& extents)
{
    CubeLutStripRequest request = base;
    request.Source = source;
    request.Content = content;
    request.Width = extents.Width;
    request.Height = source == CubeLutPreviewSource::Probes ? extents.ProbeHeight : extents.RampHeight;
    return request;
}

// A press that began on the preview and ended on it is a click, and so is Enter or Space on the
// focused preview.
void RegisterClick(UIElement* root, const std::shared_ptr<CubeLutPreview>& preview,
                   std::function<void(CubeLutPreview&)> onClick)
{
    auto armed = std::make_shared<bool>(false);
    root->RegisterEventHandler(kEventMouseDown, [armed](UIEvent&) { *armed = true; });
    root->RegisterEventHandler(kEventMouseCancel, [armed](UIEvent&) { *armed = false; });
    root->RegisterEventHandler(kEventMouseUp,
                               [armed, preview, onClick](UIEvent& e)
                               {
                                   if (!std::exchange(*armed, false) || !onClick)
                                       return;
                                   e.Stop();
                                   onClick(*preview);
                               });
    if (!onClick)
        return;
    root->SetFocusable(true);
    root->RegisterEventHandler(kEventKeyDown,
                               [preview, onClick](UIEvent& e)
                               {
                                   if (e.Mods != 0 || (e.Key != Input::kKeyCode_Enter && e.Key != Input::kKeyCode_Space))
                                       return;
                                   e.Stop();
                                   onClick(*preview);
                               });
}
} // namespace

std::shared_ptr<CubeLutPreview> CubeLutPreview::Build(UIElement* parent, const CubeLutStripRequest& request,
                                                      bool showLut, CubeLutPreviewSize size,
                                                      std::function<void(CubeLutPreview&)> onClick)
{
    if (!parent)
        return nullptr;
    const PreviewExtents& extents = ExtentsFor(size);
    std::shared_ptr<CubeLutPreview> preview(new CubeLutPreview());
    preview->m_Request = request;
    preview->m_ShowLut = showLut;

    auto root = std::make_unique<UIElement>();
    root->AddClass("cube-lut-strip");
    root->AddClass(extents.RootClass);
    root->RequestSubtreeStyleAssetPath(kCubeLutPreviewStyleAssetPath, "editor");
    root->SetTooltip(size == CubeLutPreviewSize::Enlarged ? "Click or press Escape to close"
                                                          : "Click, or press Enter, to enlarge");
    UIElement* rootRaw = root.get();
    preview->m_Root = rootRaw;

    AddBand(rootRaw, BandRequest(request, CubeLutPreviewSource::Ramp, CubeLutStripContent::LookAlone, extents), "Look",
            extents.RampBandClass, "The exposure ramp through the look alone");
    if (showLut)
    {
        const CubeLutStripRequest lutRamp =
            BandRequest(request, CubeLutPreviewSource::Ramp, CubeLutStripContent::WithLut, extents);
        preview->m_LutBands.emplace_back(
            AddBand(rootRaw, lutRamp, "+ LUT", extents.RampBandClass, "The exposure ramp through the look and this LUT"),
            lutRamp);
    }
    AddRampScale(rootRaw, size);
    AddBand(rootRaw, BandRequest(request, CubeLutPreviewSource::Probes, CubeLutStripContent::LookAlone, extents),
            "Look", extents.ProbeBandClass,
            ProbesTooltip("the look alone").c_str());
    if (showLut)
    {
        const CubeLutStripRequest lutProbes =
            BandRequest(request, CubeLutPreviewSource::Probes, CubeLutStripContent::WithLut, extents);
        preview->m_LutBands.emplace_back(
            AddBand(rootRaw, lutProbes, "+ LUT", extents.ProbeBandClass,
                    ProbesTooltip("the look and this LUT").c_str()),
            lutProbes);
    }
    AddProbeNames(rootRaw, size);

    // The root's handler holds the preview: it lives exactly as long as the elements it names.
    RegisterClick(rootRaw, preview, std::move(onClick));
    parent->AddChild(std::move(root));
    return preview;
}

CubeLutPreview::~CubeLutPreview()
{
    if (m_Enlarged)
        m_Enlarged->CloseForGoneOwner();
}

void CubeLutPreview::SetIntensity(float intensity)
{
    m_Request.Intensity = Math::Clamp01(intensity);
    for (auto& [band, request] : m_LutBands)
    {
        request.Intensity = m_Request.Intensity;
        BindBandTexture(*band, request);
    }
    if (m_Enlarged)
        m_Enlarged->SetIntensity(m_Request.Intensity);
}

void CubeLutPreview::OpenEnlarged()
{
    if (m_Enlarged || !m_Root)
        return;
    m_Enlarged = CubeLutPreviewPopover::Open(*this);
}

} // namespace GameEngine
