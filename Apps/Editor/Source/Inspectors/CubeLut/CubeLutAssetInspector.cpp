#include "Inspectors/CubeLut/CubeLutAssetInspector.h"

#include "InspectorRegistry.h"

#include "AssetCore/AssetTypes.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/CubeLutAsset.h"
#include "Assets/CubeLutColorSettings.h"
#include "Assets/RuntimeAssetMetadata.h"
#include "Components/Rendering/Look.h"
#include "Components/Rendering/PostProcessEffects/CubeLutEffect.h"
#include "Core/Engine.h"
#include "Inspectors/CubeLut/CubeLutPreview.h"
#include "Inspectors/InspectorUIHelpers.h"

#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>

namespace GameEngine
{
namespace
{
using Rendering::CubeLutInputEncoding;

static constexpr EnumEntry<CubeLutInputEncoding> kInputEncodings[] = {
    {CubeLutInputEncoding::Rec709SRGB, "Rec.709 / sRGB"},
    {CubeLutInputEncoding::Linear, "Linear"},
    {CubeLutInputEncoding::ArriLogC3, "ARRI LogC3"},
    {CubeLutInputEncoding::DaVinciIntermediate, "DaVinci Intermediate"},
    {CubeLutInputEncoding::ACEScct, "ACEScct"},
    {CubeLutInputEncoding::Cineon, "Cineon"},
};

// The preview's owner: one asset inspector is shown at a time, and no entity's inspector uses this
// id (theirs are entity ids + 1).
constexpr uint32_t kAssetInspectorPreviewOwner = std::numeric_limits<uint32_t>::max();

std::string FormatNumber(float value)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%g", static_cast<double>(value));
    return text;
}

std::string TableText(const CubeLutFileHeader& header)
{
    std::string text;
    if (header.Size1D > 0)
        text = "1D shaper of " + std::to_string(header.Size1D);
    if (header.Size3D > 0)
        text += (text.empty() ? "" : " + ") + std::string("3D, ") + std::to_string(header.Size3D) + " per side";
    return text;
}

void AddValueRow(UIElement* parent, const char* label, const std::string& value, const char* tooltip)
{
    UIElement* row = InspectorUI::AddRow(parent);
    InspectorUI::AddLabel(row, label, tooltip);
    InspectorUI::AddTextBlock(InspectorUI::AddFieldContainer(row), value, "inspector-text");
}

// The caption under the encoding: what the encoding is, where its value comes from, and how a choice
// goes back to the file's proposal.
void AddSourceCaption(UIElement* parent, bool chosen, std::string_view proposedLabel, const std::string& reason)
{
    const std::string proposed(proposedLabel);
    const std::string source = chosen ? "Chosen here; the file proposes " + proposed + " (" + reason + "). Choose " +
                                            proposed + " to follow the file again."
                                      : "Proposed from the file: " + reason + ".";
    InspectorUI::AddTextBlock(InspectorUI::AddActionRow(parent, "cube-lut-source-row"),
                              "The encoding the table's input is authored in. " + source, "inspector-field-caption");
}

// Stores a choice on the asset and reloads it, so the frame, the previews and this inspector read
// it. Choosing the proposal stores nothing: the asset follows its file again.
void StoreEncoding(const std::filesystem::path& path, CubeLutInputEncoding encoding, bool isProposal)
{
    AssetManager& assets = EngineCore::GetInstance().GetAssetManager();
    const std::string value = isProposal ? std::string{} : std::string(Rendering::CubeLutInputEncodingKey(encoding));
    if (!assets.GetRegistry().SetMetaValue(path, kCubeLutInputEncodingMetaKey, value))
        return;
    const GUID guid = assets.ResolveAssetGuid(path);
    if (!guid.IsNull())
        assets.RequestAsyncReload(guid);
}
} // namespace

void RegisterCubeLutAssetInspector()
{
    InspectorFn fn = [](const InspectorContext& ctx)
    {
        auto* asset = static_cast<CubeLutAsset*>(ctx.Object);
        if (!ctx.Parent || !asset || !EngineCore::GetInstance().IsInitialized())
            return;

        const std::filesystem::path path = asset->GetPath();
        const CubeLutFileHeader& header = asset->GetHeader();
        const Rendering::CubeLutColorProposal& proposal = asset->GetProposal();
        AssetRegistry& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();
        const CubeLutColorSettings settings = ResolveCubeLutColorSettings(registry, path, proposal);
        const AssetImportSettingsOrigin origin = registry.GetImportSettingsOrigin(path);

        if (!header.Title.empty())
            AddValueRow(ctx.Parent, "Title", header.Title, "The file's TITLE line");
        AddValueRow(ctx.Parent, "Table", TableText(header), "The file's LUT_1D_SIZE and LUT_3D_SIZE");
        AddValueRow(ctx.Parent, "Input Range", FormatNumber(header.InputMin) + " to " + FormatNumber(header.InputMax),
                    "The input values the tables cover (DOMAIN_MIN/MAX or LUT_*_INPUT_RANGE)");

        auto* encoding = InspectorUI::AddEnumRow(ctx.Parent, "Input Encoding", kInputEncodings, settings.InputEncoding,
            "The encoding the LUT's input is authored in: the frame's colors are converted into it before the "
            "lookup and back after it.");
        encoding->SetOnValueChanged([path, proposed = proposal.InputEncoding](CubeLutInputEncoding v) {
            StoreEncoding(path, v, v == proposed);
        });
        AddSourceCaption(ctx.Parent, settings.InputEncodingChosen,
                         Rendering::CubeLutInputEncodingLabel(proposal.InputEncoding), proposal.Reason);

        if (!origin.Writable)
        {
            const std::string reason = "Read-only here: set it in the package " + origin.SourceAlias + ".";
            InspectorUI::DisableRowOfControl(encoding, reason.c_str());
        }

        // The preview through the Neutral look, at full intensity and at the stage the table is made
        // for: Finish for a display LUT, Grade for a log or scene LUT, which at Finish would be fed
        // display values it was not made for.
        const bool displayLut = Rendering::CubeLutExpectsDisplayInput(settings.InputEncoding, header.InputMax);
        CubeLutStripRequest request{};
        request.Lut = asset->GetGUID();
        request.Owner = kAssetInspectorPreviewOwner;
        request.Intensity = 1.0f;
        request.Look = static_cast<int32_t>(Components::Look::Neutral);
        request.Stage =
            static_cast<uint32_t>(displayLut ? Components::CubeLutStage::Finish : Components::CubeLutStage::Grade);
        InspectorUI::AddTextBlock(InspectorUI::AddActionRow(ctx.Parent, "inspector-caption-row"),
                                  displayLut ? "Preview: the Neutral look, the LUT at Finish, where a display LUT runs."
                                             : "Preview: the Neutral look, the LUT at Grade, where a log or scene LUT runs.",
                                  "inspector-field-caption");
        UIElement* field = InspectorUI::AddActionRow(ctx.Parent, "cube-lut-strip-row");
        (void)CubeLutPreview::Build(field, request, true, CubeLutPreviewSize::Inspector,
                                    [](CubeLutPreview& preview) { preview.OpenEnlarged(); });
    };

    InspectorRegistry::Get().RegisterAssetInspector(AssetType::CubeLut, std::move(fn));
}

} // namespace GameEngine
