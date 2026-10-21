#include "Inspectors/CubeLutStageNotice.h"

#include "InspectorRegistry.h"

#include "AssetCore/GUID.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/CubeLutAsset.h"
#include "Assets/CubeLutColorSettings.h"
#include "Components/Rendering/PostProcessEffects/CubeLutEffect.h"
#include "Core/Engine.h"
#include "Rendering/Utils/CubeLutColorDomain.h"
#include "UI/Controls/InspectorNotice.h"

#include <cstdio>
#include <memory>
#include <utility>

namespace GameEngine
{
namespace
{
using Components::CubeLutStage;
using Rendering::CubeLutInputEncoding;

// "Linear, 0 to 1" for a linear table, which its range makes a display or a scene LUT; the encoding's
// label for any other.
std::string InputText(CubeLutInputEncoding encoding, float inputMax)
{
    if (encoding != CubeLutInputEncoding::Linear)
        return std::string(Rendering::CubeLutInputEncodingLabel(encoding));
    char range[32];
    std::snprintf(range, sizeof(range), "%g", static_cast<double>(inputMax));
    return std::string("Linear, 0 to ") + range;
}

} // namespace

std::string CubeLutStageNoticeText(std::string_view fileName, CubeLutInputEncoding encoding, float inputMax,
                                   CubeLutStage stage)
{
    const bool displayLut = Rendering::CubeLutExpectsDisplayInput(encoding, inputMax);
    const std::string head = std::string(fileName) + " is a ";
    const std::string input = " (" + InputText(encoding, inputMax) + " input): ";
    if (displayLut && stage == CubeLutStage::Grade)
        return head + "display LUT" + input + "at Grade it clips scene color above 1.";
    if (!displayLut && stage == CubeLutStage::Finish)
        return head + (encoding == CubeLutInputEncoding::Linear ? "scene LUT" : "log LUT") + input +
               "at Finish it receives display color and crushes the image.";
    return {};
}

void AddCubeLutStageNotice(const InspectorContext& ctx, const GUID& lutGuid, CubeLutStage stage,
                           std::function<void(CubeLutStage)> chooseStage)
{
    AssetManager& assets = EngineCore::GetInstance().GetAssetManager();
    if (const auto asset = std::dynamic_pointer_cast<CubeLutAsset>(assets.GetAsset(lutGuid)))
    {
        const CubeLutInputEncoding encoding =
            ResolveCubeLutColorSettings(assets.GetRegistry(), asset->GetPath(), asset->GetProposal()).InputEncoding;
        // GetName keeps the file's spelling; the path is the registry's case-folded key.
        const std::string fileName = asset->GetName() + asset->GetPath().extension().string();
        const std::string text = CubeLutStageNoticeText(fileName, encoding, asset->GetHeader().InputMax, stage);
        if (text.empty() || !ctx.Parent)
            return;
        // The notice proposes the other stage: the one the LUT is made for.
        const bool toFinish = stage == CubeLutStage::Grade;
        auto notice = std::make_unique<EditorUI::InspectorNotice>(text);
        // No callback, no button: an action that does nothing is not offered.
        if (chooseStage)
            notice->SetAction(toFinish ? "Choose Finish" : "Choose Grade",
                              toFinish ? "Run the LUT after the look, where a display LUT belongs"
                                       : "Run the LUT before the look, where a log or scene LUT belongs",
                              [chooseStage = std::move(chooseStage), toFinish]
                              { chooseStage(toFinish ? CubeLutStage::Finish : CubeLutStage::Grade); });
        ctx.Parent->AddChild(std::move(notice));
        return;
    }
    if (!ctx.Parent || !ctx.FrameRefreshCallbacks || !ctx.RequestInspectorRefresh)
        return;
    // The handle stays with the frame callback, so a failed load ends the polling too. The refresh is
    // posted: it rebuilds the inspector, which clears the callbacks being run.
    auto load = std::make_shared<AssetLoadHandle>(
        assets.LoadAsset(lutGuid, AssetLoadResultCallback{}, AssetLoadPriority::High));
    auto completed = std::make_shared<bool>(false);
    ctx.FrameRefreshCallbacks->push_back(
        [load, completed, host = ctx.Parent, refresh = ctx.RequestInspectorRefresh]()
        {
            if (*completed || !load->IsComplete())
                return;
            *completed = true;
            if (load->GetResult())
                host->PostAction(refresh);
        });
}

} // namespace GameEngine
