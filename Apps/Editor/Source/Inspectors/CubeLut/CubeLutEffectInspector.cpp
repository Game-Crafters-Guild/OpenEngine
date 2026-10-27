#include "Inspectors/CubeLut/CubeLutEffectInspector.h"

#include "InspectorRegistry.h"

#include "AssetCore/AssetTypes.h"
#include "AssetCore/GUID.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Components/Rendering/PostProcessEffects/CubeLutEffect.h"
#include "Components/Rendering/PostProcessVolume.h"
#include "Core/Engine.h"
#include "EditorChangeNotifications.h"
#include "Editor/Entities/EditorECSHelpers.h"
#include "Inspectors/CubeLut/CubeLutPreview.h"
#include "Inspectors/CubeLut/CubeLutStageNotice.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Mathematics/Interpolation.h"
#include "UI/AssetField.h"

#include <algorithm>
#include <memory>

namespace GameEngine
{
namespace
{
using Components::CubeLutStage;

static constexpr EnumEntry<CubeLutStage> kStages[] = {
    {CubeLutStage::Finish, "Finish (after the look)"},
    {CubeLutStage::Grade, "Grade (before the look)"},
};

// The LUT's preview under the asset row: the exposure ramp and the saturated probes through the
// volume's look (top of each pair) and through the look and the LUT at its stage and intensity
// (bottom), so the difference is the LUT. A disabled effect grades nothing, so its preview shows
// the look alone. A click enlarges it.
std::shared_ptr<CubeLutPreview> AddLutPreview(UIElement* parent, ECS::EntityHandle entity,
                                              const Components::CubeLutEffect& effect, Components::Look look)
{
    UIElement* field = InspectorUI::AddActionRow(parent, "cube-lut-strip-row");
    if (!field)
        return nullptr;
    CubeLutStripRequest request{};
    request.Lut = effect.LutAssetGuid.ToGuid();
    request.Owner = entity.id + 1u;
    request.Intensity = Math::Clamp01(effect.Intensity);
    request.Look = static_cast<int32_t>(look);
    request.Stage = static_cast<uint32_t>(effect.Stage);
    return CubeLutPreview::Build(field, request, effect.Enabled, CubeLutPreviewSize::Inspector,
                                 [](CubeLutPreview& preview) { preview.OpenEnlarged(); });
}

// One undoable change of the effect's stage: the Stage row's and the stage notice's action. The
// stage decides whether the notice shows, so the section is rebuilt after the commit; a plain
// commit leaves the inspector as it is, and a notice's button would leave the old stage and the
// notice on screen.
void CommitStage(ECS::World* world, ECS::EntityHandle entity, Editor::EditorChangeNotifications* notifications,
                 Editor::UndoRedoService* undo, CubeLutStage stage)
{
    InspectorDrag::CommitComponentWithUndo<Components::CubeLutEffect>(
        world, entity, notifications, undo, "Change LUT Stage",
        [stage](Components::CubeLutEffect& effect) { effect.Stage = stage; });
    if (!notifications)
        return;
    Editor::EditorChangeNotifications::ComponentChangedEvent rebuild{};
    rebuild.world = world;
    rebuild.entity = entity;
    rebuild.componentType = ECS::GetComponentTypeId<Components::CubeLutEffect>();
    rebuild.kind = Editor::EditorChangeNotifications::ChangeKind::InspectorRebuild;
    notifications->NotifyComponentChanged(rebuild);
}

} // namespace

void RegisterCubeLutEffectInspector()
{
    using namespace InspectorDrag;
    using LUT = Components::CubeLutEffect;

    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;

        auto* effect = ctx.World->GetComponent<LUT>(ctx.Entity);
        if (!effect)
            return;

        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
        Editor::UndoRedoService* undo = ctx.Undo;

        {
            UIElement* row = InspectorUI::AddRow(ctx.Parent);
            Label* label = InspectorUI::AddLabel(row, "LUT", "Resolve .cube asset; its input encoding is set on the asset");
            if (label) label->AddClass("inspector-label-no-drag");
            UIElement* fieldContainer = InspectorUI::AddFieldContainer(row);

            auto field = std::make_unique<AssetField>();
            field->AddClass("dropdown-asset-field");
            field->SetAcceptedTypes({AssetType::CubeLut});
            if (auto* engine = &EngineCore::GetInstance(); engine->IsInitialized())
                field->SetAssetRegistry(&engine->GetAssetManager().GetRegistry());
            field->SetValue(effect->LutAssetGuid.ToGuid());
            field->SetOnValueChanged([w, e, n, undo](const GUID& guid) {
                CommitComponentWithUndo<LUT>(w, e, n, undo, "Change LUT Asset",
                    [guid](LUT& u) {
                        u.LutAssetGuid.Set(guid);
                    });
            });
            fieldContainer->AddChild(std::move(field));
        }

        std::shared_ptr<CubeLutPreview> preview;
        if (!effect->LutAssetGuid.IsNull())
        {
            const auto* volume = w->GetComponent<Components::PostProcessVolume>(e);
            preview = AddLutPreview(ctx.Parent, e, *effect, volume ? volume->Look : Components::Look::Neutral);
        }

        auto* stage = InspectorUI::AddEnumRow(ctx.Parent, "Stage", kStages, effect->Stage,
            "Finish grades the look's output, as a display LUT expects. Grade runs the LUT before the look, "
            "on the scene's colors, as a colorist's log or scene LUT expects.");
        stage->SetOnValueChanged([w, e, n, undo](CubeLutStage v) { CommitStage(w, e, n, undo, v); });
        if (!effect->LutAssetGuid.IsNull())
            AddCubeLutStageNotice(ctx, effect->LutAssetGuid.ToGuid(), effect->Stage,
                [w, e, n, undo](CubeLutStage v) { CommitStage(w, e, n, undo, v); });

        AddComponentFloatRowWithDrag<LUT>(ctx.Parent, "Intensity",
            Math::Clamp01(effect->Intensity), w, e, n, undo,
            "Change LUT Intensity",
            [preview](LUT& u, float v) {
                u.Intensity = Math::Clamp01(v);
                if (preview)
                    preview->SetIntensity(u.Intensity);
            },
            1.0f,
            "0 = bypass, 1 = full LUT blend.",
            {},
            0.0f,
            1.0f);
    };

    InspectorRegistry::Get().RegisterComponentInspector<LUT>(std::move(fn));
}

} // namespace GameEngine
