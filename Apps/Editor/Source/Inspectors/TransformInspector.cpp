#include "Inspectors/TransformInspector.h"

#include "InspectorRegistry.h"
#include "Platform/SystemMetrics.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorOverrideRows.h"

#include "EditorChangeNotifications.h"
#include "Editor/Entities/EditorECSHelpers.h"
#include "UndoRedo/MultiEntityComponentSnapshot.h"
#include "UndoRedo/UndoRedoService.h"

#include "UI/EditorIcons.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Vector3Field.h"
#include "UI/InspectorSection.h"
#include "UI/Interaction/ContextMenuManipulator.h"
#include "UI/StyleProperties.h"

#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"

#include "Components/Hierarchy.h"
#include "Components/Transform.h"
#include "Mathematics/Quaternion.h"
#include "Mathematics/Vector3.h"
#include "Mathematics/WorldPosition.h"
#include "Types/FormatNumber.h"
#include "Types/ParseNumber.h"
#include "Types/StringUtils.h"

#include "EditorContextMenu/UIContextMenu.h"
#include "Platform/Capabilities.h"
#include "Platform/ContextMenu.h"
#include "Platform/Clipboard.h"

#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{

using namespace Components;
using GameEngine::Mathematics::Quaternion;
using GameEngine::Mathematics::Vector3;

namespace
{

struct TRS
{
    Vector3 position;
    Quaternion rotation;
    Vector3 scale{1.0f, 1.0f, 1.0f};
};

inline TRS IdentityTRS()
{
    return TRS{};
}

inline TRS GetLocalTRS(ECS::Entity& e)
{
    TRS t = IdentityTRS();
    if (const Transform* xf = e.Get<Transform>())
    {
        t.position = xf->GetPosition() - Mathematics::WorldPosition{};
        t.rotation = xf->GetRotation();
        t.scale = xf->GetScale();
    }
    return t;
}

inline TRS CombineTRS(const TRS& parent, const TRS& local)
{
    TRS out{};
    out.scale = Vector3(
        parent.scale.x * local.scale.x,
        parent.scale.y * local.scale.y,
        parent.scale.z * local.scale.z);

    out.rotation = parent.rotation * local.rotation;

    Vector3 scaledLocal(
        local.position.x * parent.scale.x,
        local.position.y * parent.scale.y,
        local.position.z * parent.scale.z);
    Vector3 rotated = parent.rotation.Rotate(scaledLocal);
    out.position = parent.position + rotated;

    return out;
}

TRS ComputeWorldTRSForEntity(ECS::World* world, ECS::EntityHandle handle)
{
    if (!world)
    {
        return IdentityTRS();
    }

    ECS::Entity e(world, handle);
    if (!e.IsValid())
    {
        return IdentityTRS();
    }

    TRS local = GetLocalTRS(e);

    if (auto* parentComp = e.Get<Parent>())
    {
        ECS::Entity parentEntity(world, parentComp->Entity);
        if (parentEntity.IsValid())
        {
            TRS parentWorld = ComputeWorldTRSForEntity(world, parentComp->Entity);
            return CombineTRS(parentWorld, local);
        }
    }

    // No valid parent: world == local
    return local;
}

void GetParentWorldTRS(ECS::World* world, ECS::EntityHandle handle,
                       Vector3& outPos, Quaternion& outRot, Vector3& outScale)
{
    TRS identity = IdentityTRS();
    outPos = identity.position;
    outRot = identity.rotation;
    outScale = identity.scale;

    if (!world)
    {
        return;
    }

    ECS::Entity e(world, handle);
    if (!e.IsValid())
    {
        return;
    }

    auto* parentComp = e.Get<Parent>();
    if (!parentComp)
    {
        return; // identity parent
    }

    TRS parentWorld = ComputeWorldTRSForEntity(world, parentComp->Entity);
    outPos = parentWorld.position;
    outRot = parentWorld.rotation;
    outScale = parentWorld.scale;
}

Vector3 ComputeLocalPositionFromWorld(const Vector3& worldPos,
                                       const Vector3& parentPos,
                                       const Quaternion& parentRot,
                                       const Vector3& parentScale)
{
    Vector3 delta = worldPos - parentPos;
    Vector3 unrotated = parentRot.Conjugated().Rotate(delta);

    return Vector3(
        (parentScale.x != 0.0f) ? (unrotated.x / parentScale.x) : unrotated.x,
        (parentScale.y != 0.0f) ? (unrotated.y / parentScale.y) : unrotated.y,
        (parentScale.z != 0.0f) ? (unrotated.z / parentScale.z) : unrotated.z);
}

using InspectorUI::ContainsFocusedElement;

static bool TryParseAllTRSFromClipboard(const std::string& text,
                                        Rendering::Vector3& outPos,
                                        Rendering::Vector3& outRotEulerDeg,
                                        Rendering::Vector3& outScale)
{
    // Walk the text and use strtof to extract numbers in order. This naturally
    // skips letters in labels ("Position", "Rotation", "Scale") and still
    // accepts scientific notation like "1e-3" inside a number.
    float n[9];
    int got = 0;
    const char* p = text.c_str();
    const char* end = p + text.size();
    while (p < end && got < 9)
    {
        // Find a position that could start a number (digit, '.', '+', or '-').
        while (p < end)
        {
            const char c = *p;
            const bool digit = (c >= '0' && c <= '9');
            const bool isStart = digit || c == '.' || c == '+' || c == '-';
            if (isStart) break;
            ++p;
        }
        if (p >= end) break;

        char* nextp = nullptr;
        errno = 0;
        const float v = std::strtof(p, &nextp);
        if (nextp == p)
        {
            // Not actually a number (e.g. lone '+' or '.'); skip one char and retry.
            ++p;
            continue;
        }
        n[got++] = v;
        p = nextp;
    }
    if (got != 9)
        return false;
    outPos = Rendering::Vector3(n[0], n[1], n[2]);
    outRotEulerDeg = Rendering::Vector3(n[3], n[4], n[5]);
    outScale = Rendering::Vector3(n[6], n[7], n[8]);
    return true;
}

class TransformInspectorSection final : public UIElement
{
public:
    explicit TransformInspectorSection(const InspectorContext& ctx)
        : m_World(ctx.World)
        , m_Entity(ctx.Entity)
        , m_Undo(ctx.Undo)
        , m_Changes(ctx.ChangeNotifications)
        , m_Window(ctx.Window)
    {
        // Store additional entities for multi-edit (skip the primary).
        for (const auto& e : ctx.Entities)
        {
            if (e != ctx.Entity && e.IsValid())
                m_AdditionalEntities.push_back(e);
        }

        AddClass("inspector-section");
        AddClass("inspector-section-transform");

        BuildUI(ctx);
        RefreshFromWorld();
        Subscribe();

        if (ctx.SimulationRefreshCallbacks)
        {
            // Simulation polling can observe a gizmo-written Transform before
            // its Preview notification reaches this section. Use the same
            // throttled refresh in both paths.
            ctx.SimulationRefreshCallbacks->push_back(
                [this]() { RefreshPreviewFromWorld(); });
        }

        if (ctx.Section)
        {
            ctx.Section->SetOnHeaderContextMenu([this](float x, float y) {
                ShowSectionContextMenu(x, y);
            });
        }
    }

    ~TransformInspectorSection() override
    {
        if (m_ActiveEdit)
        {
            m_ActiveEdit.Commit();
        }
        if (m_Changes && m_Sub)
        {
            m_Changes->Unsubscribe(m_Sub);
        }
    }

    void OnPostLayout() override
    {
        UIElement::OnPostLayout();
        const float w = GetLayoutWidth();
        if (w <= 1.0f)
            return;

        constexpr float kWideMinWidthPx = 300.0f;
        const bool wide = w >= kWideMinWidthPx;
        if (m_TransformWideLayoutInitialized && wide == m_TransformWideLayout)
            return;

        m_TransformWideLayoutInitialized = true;
        m_TransformWideLayout = wide;

        if (wide)
            AddClass("inspector-transform-wide");
        else
            RemoveClass("inspector-transform-wide");
    }

private:
    // A row with a label and a Vector3 field. `overrideField` is the Inspector field of the row, as
    // InspectorFieldForSceneKey names it: the reflected field ("Position") or the scene key ("rotation");
    // empty for a derived row, which has the row menu and no override marks.
    void AddTrsVector3Row(const InspectorContext& context,
                          std::string overrideField,
                          const std::string& labelText,
                          const std::string& tooltip,
                          Vector3Field*& outField,
                          const std::function<void(Vector3Field*)>& configureField,
                          const std::function<void(const Rendering::Vector3&)>& onChanging,
                          const std::function<void(const Rendering::Vector3&)>& onChanged)
    {
        auto row = std::make_unique<UIElement>();
        row->AddClass("inspector-transform-trs-row");

        auto label = std::make_unique<Label>();
        Label* labelPtr = label.get();
        label->AddClass("inspector-field-label");
        label->SetText(labelText);
        label->SetTooltip(tooltip);

        auto field = std::make_unique<Vector3Field>();
        outField = field.get();
        field->AddClass("inspector-vector3");
        configureField(field.get());

        UIElement* rowPtr = row.get();
        row->AddChild(std::move(label));
        row->AddChild(std::move(field));
        AddChild(std::move(row));

        SetupLabelDrag(labelPtr, outField, onChanging, onChanged);
        std::vector<ContextMenuManipulator::Item> rowItems = RowMenuItems(outField, onChanged);
        if (overrideField.empty())
            rowPtr->AddManipulator(ContextMenuManipulator::Create(std::move(rowItems)));
        else
            BindFieldRow(context, ECS::GetComponentTypeId<Transform>(), rowPtr, std::move(overrideField),
                         std::move(rowItems));
    }

    void BuildUI(const InspectorContext& ctx)
    {
        AddTrsVector3Row(
            ctx, "Position",
            "Local Position",
            "Position relative to parent (or world origin if no parent)",
            m_LocalPos,
            [this](Vector3Field* f) {
                f->ConfigureComponentIds("Transform.LocalPosition");
                f->SetDefaultValue(Rendering::Vector3(0.0f, 0.0f, 0.0f));
                f->SetOnValueChanging([this](const Rendering::Vector3& v) { OnLocalPositionChanging(v); });
                f->SetOnValueChanged([this](const Rendering::Vector3& v) { OnLocalPositionChanged(v); });
            },
            [this](const Rendering::Vector3& v) { OnLocalPositionChanging(v); },
            [this](const Rendering::Vector3& v) { OnLocalPositionChanged(v); });

        AddTrsVector3Row(
            ctx, "rotation",
            "Local Rotation",
            "Rotation relative to parent in Euler angles (degrees, XYZ order)",
            m_LocalRot,
            [this](Vector3Field* f) {
                f->ConfigureComponentIds("Transform.LocalRotation");
                f->SetDefaultValue(Rendering::Vector3(0.0f, 0.0f, 0.0f));
                f->SetOnValueChanging([this](const Rendering::Vector3& v) { OnLocalRotationChanging(v); });
                f->SetOnValueChanged([this](const Rendering::Vector3& v) { OnLocalRotationChanged(v); });
            },
            [this](const Rendering::Vector3& v) { OnLocalRotationChanging(v); },
            [this](const Rendering::Vector3& v) { OnLocalRotationChanged(v); });

        AddTrsVector3Row(
            ctx, "scale",
            "Local Scale",
            "Scale relative to parent (1, 1, 1 = no scaling)",
            m_LocalScale,
            [this](Vector3Field* f) {
                f->ConfigureComponentIds("Transform.LocalScale");
                f->SetDefaultValue(Rendering::Vector3(1.0f, 1.0f, 1.0f));
                f->SetOnValueChanging([this](const Rendering::Vector3& v) { OnLocalScaleChanging(v); });
                f->SetOnValueChanged([this](const Rendering::Vector3& v) { OnLocalScaleChanged(v); });
            },
            [this](const Rendering::Vector3& v) { OnLocalScaleChanging(v); },
            [this](const Rendering::Vector3& v) { OnLocalScaleChanged(v); });

        AddTrsVector3Row(
            ctx, {},
            "World Position",
            "World-space position (derived from hierarchy); editing converts back to local space",
            m_WorldPos,
            [this](Vector3Field* f) {
                f->ConfigureComponentIds("Transform.WorldPosition");
                f->SetDefaultValue(Rendering::Vector3(0.0f, 0.0f, 0.0f));
                f->SetOnValueChanging([this](const Rendering::Vector3& v) { OnWorldPositionChanging(v); });
                f->SetOnValueChanged([this](const Rendering::Vector3& v) { OnWorldPositionChanged(v); });
            },
            [this](const Rendering::Vector3& v) { OnWorldPositionChanging(v); },
            [this](const Rendering::Vector3& v) { OnWorldPositionChanged(v); });
    }

    void Subscribe()
    {
        if (!m_Changes || !m_World || !m_Entity.IsValid())
        {
            return;
        }

        const ECS::ComponentTypeId transformType = ECS::GetComponentTypeId<Transform>();
        m_Sub = m_Changes->SubscribeComponentChanged(
            [this](const Editor::EditorChangeNotifications::ComponentChangedEvent& e)
            {
                if (e.world != m_World)
                {
                    return;
                }
                if (e.entity != m_Entity)
                {
                    return;
                }
                if (e.componentType != transformType)
                {
                    return;
                }

                // Keep numeric values live at 30 Hz without tying text updates
                // to an uncapped viewport. SetValue detects the fixed-size text
                // boxes and skips layout automatically.
                if (e.kind == Editor::EditorChangeNotifications::ChangeKind::Preview)
                {
                    RefreshPreviewFromWorld();
                    return;
                }

                m_LastPreviewRefresh = {};
                RefreshFromWorld();
            });
    }

    void RefreshPreviewFromWorld()
    {
        using clock = std::chrono::steady_clock;
        const auto now = clock::now();
        constexpr auto kPreviewInterval = std::chrono::milliseconds(33);
        if (m_LastPreviewRefresh.time_since_epoch().count() != 0 &&
            now - m_LastPreviewRefresh < kPreviewInterval)
        {
            return;
        }

        m_LastPreviewRefresh = now;
        RefreshFromWorld();
    }

    void RefreshFromWorld()
    {
        if (!m_World || !m_Entity.IsValid() || !m_World->IsValid(m_Entity))
        {
            return;
        }

        auto* t = m_World->GetComponent<Transform>(m_Entity);
        if (!t)
        {
            return;
        }

        Vector3 localPos = t->GetPosition() - Mathematics::WorldPosition{};
        Quaternion localRot = t->GetRotation();
        Vector3 localScale = t->GetScale();

        TRS worldTrs = ComputeWorldTRSForEntity(m_World, m_Entity);
        Vector3 worldPos = worldTrs.position;

        float32 xDeg = 0.0f, yDeg = 0.0f, zDeg = 0.0f;
        EulerXYZDegreesFromQuaternion(localRot, xDeg, yDeg, zDeg);

        UIManager* ui = GetOwnerManager();
        static const std::string kEmpty;
        const std::string& focusId = ui ? ui->GetFocusedElementId() : kEmpty;

        auto vecChanged = [](const Rendering::Vector3& a, const Rendering::Vector3& b) -> bool
        {
            constexpr float eps = 1.0e-4f;
            return (std::fabs(a.x - b.x) > eps) || (std::fabs(a.y - b.y) > eps) || (std::fabs(a.z - b.z) > eps);
        };

        const Rendering::Vector3 localPosV(localPos.x, localPos.y, localPos.z);
        const Rendering::Vector3 localRotV(xDeg, yDeg, zDeg);
        const Rendering::Vector3 localScaleV(localScale.x, localScale.y, localScale.z);
        const Rendering::Vector3 worldPosV(worldPos.x, worldPos.y, worldPos.z);

        if (m_LocalPos && !ContainsFocusedElement(m_LocalPos, focusId) &&
            (!m_HasLastLocalPos || vecChanged(localPosV, m_LastLocalPos)))
        {
            m_LocalPos->SetValue(localPosV);
            m_LastLocalPos = localPosV;
            m_HasLastLocalPos = true;
        }
        if (m_LocalRot && !ContainsFocusedElement(m_LocalRot, focusId) &&
            (!m_HasLastLocalRot || vecChanged(localRotV, m_LastLocalRot)))
        {
            m_LocalRot->SetValue(localRotV);
            m_LastLocalRot = localRotV;
            m_HasLastLocalRot = true;
        }
        if (m_LocalScale && !ContainsFocusedElement(m_LocalScale, focusId) &&
            (!m_HasLastLocalScale || vecChanged(localScaleV, m_LastLocalScale)))
        {
            m_LocalScale->SetValue(localScaleV);
            m_LastLocalScale = localScaleV;
            m_HasLastLocalScale = true;
            m_LocalScaleLinkedLastValue = localScaleV;
            m_HasLocalScaleLinkedLastValue = true;
        }
        if (m_WorldPos && !ContainsFocusedElement(m_WorldPos, focusId) &&
            (!m_HasLastWorldPos || vecChanged(worldPosV, m_LastWorldPos)))
        {
            m_WorldPos->SetValue(worldPosV);
            m_LastWorldPos = worldPosV;
            m_HasLastWorldPos = true;
        }
    }

    // Every entity a transform edit can touch: the primary plus the rest of the
    // selection, minus any that lost validity or their Transform since selection.
    // Snapshotting the whole set is what makes a multi-entity edit undo in one step.
    std::vector<ECS::EntityHandle> CollectEditTargets() const
    {
        std::vector<ECS::EntityHandle> targets;
        if (!m_World)
            return targets;

        targets.reserve(m_AdditionalEntities.size() + 1u);
        auto addIfEditable = [this, &targets](ECS::EntityHandle entity)
        {
            if (!entity.IsValid() || !m_World->IsValid(entity))
                return;
            if (!m_World->GetComponent<Transform>(entity))
                return;
            targets.push_back(entity);
        };

        addIfEditable(m_Entity);
        for (const auto& ent : m_AdditionalEntities)
            addIfEditable(ent);
        return targets;
    }

    void EnsureEditStarted(const char* name)
    {
        if (m_ActiveEdit || !m_Undo || !m_World || !m_Entity.IsValid())
        {
            return;
        }

        std::vector<ECS::EntityHandle> targets = CollectEditTargets();
        if (targets.empty())
        {
            return;
        }

        const char* label = (name && *name) ? name : "Transform";
        m_ActiveEdit = m_Undo->BeginInteractiveEdit(
            label,
            Editor::MultiEntityUndo::MakeComponentSnapshotTarget(
                m_World,
                std::move(targets),
                ECS::GetComponentTypeId<Transform>(),
                m_Changes,
                label));
    }

    template<typename ApplyFn>
    void PreviewEdit(const char* name, ApplyFn&& applyFn)
    {
        if (!m_World || !m_Entity.IsValid())
        {
            return;
        }

        if (m_Undo)
        {
            EnsureEditStarted(name);
            if (m_ActiveEdit)
            {
                m_ActiveEdit.Preview(std::forward<ApplyFn>(applyFn));
                return;
            }
        }

        // Fallback (no undo service): still apply and emit preview notifications.
        applyFn();
        if (m_Changes)
        {
            m_Changes->NotifyComponentChange<Transform>(m_World, m_Entity, Editor::EditorChangeNotifications::ChangeKind::Preview);
            for (const auto& ent : m_AdditionalEntities)
                m_Changes->NotifyComponentChange<Transform>(m_World, ent, Editor::EditorChangeNotifications::ChangeKind::Preview);
        }
    }

    // Ends an in-flight drag/scrub interactive edit (preview already applied to the world).
    // Discrete actions such as clipboard paste must not merge into that snapshot or undo
    // would revert past the pre-paste state in one step.
    void FinalizeActiveInteractiveEditIfAny()
    {
        if (!m_Undo || !m_ActiveEdit)
        {
            ClearMultiEditBaseline();
            return;
        }
        m_ActiveEdit.Commit();
        m_ActiveEdit = {};
        ClearMultiEditBaseline();
    }

    template<typename ApplyFn>
    void CommitEdit(const char* name, ApplyFn&& applyFn)
    {
        if (!m_World || !m_Entity.IsValid())
        {
            return;
        }

        if (m_Undo)
        {
            EnsureEditStarted(name);
            if (m_ActiveEdit)
            {
                // Ensure final state is applied (commit assumes world already reflects final).
                m_ActiveEdit.Preview(std::forward<ApplyFn>(applyFn));
                m_ActiveEdit.Commit();
                m_ActiveEdit = {};
                ClearMultiEditBaseline();
                return;
            }
        }

        // Fallback (no undo service): apply and emit commit notifications.
        applyFn();
        ClearMultiEditBaseline();
        if (m_Changes)
        {
            m_Changes->NotifyComponentCommit<Transform>(m_World, m_Entity);
            for (const auto& ent : m_AdditionalEntities)
                m_Changes->NotifyComponentCommit<Transform>(m_World, ent);
        }
    }

    void ApplyLocalPositionToEntity(ECS::EntityHandle entity, const Rendering::Vector3& v)
    {
        ECS::Entity e(m_World, entity);
        if (!e.IsValid())
            return;

        if (const Transform* t = e.Get<Transform>())
        {
            Vector3 newPos(v.x, v.y, v.z);
            Quaternion r = t->GetRotation();
            Vector3 s = t->GetScale();
            Transform updated = Transform::FromTRS(Mathematics::WorldPosition::FromVector3(newPos), r, s);
            m_World->AddComponentImmediate(entity, updated);

            Vector3 parentPos;
            Quaternion parentRot;
            Vector3 parentScale{1.0f, 1.0f, 1.0f};
            GetParentWorldTRS(m_World, entity, parentPos, parentRot, parentScale);
            TRS worldTrs = CombineTRS(TRS{parentPos, parentRot, parentScale}, TRS{newPos, r, s});
            (void)Editor::TeleportPhysicsBody(m_World, entity, worldTrs.position, worldTrs.rotation);
        }
    }

    void ApplyLocalPosition(const Rendering::Vector3& v)
    {
        EnsureMultiEditBaseline();
        const Vector3 primaryTarget(v.x, v.y, v.z);
        const Vector3 delta = primaryTarget - m_MultiEditBaseline.primary.localPos;
        ApplyLocalPositionToEntity(m_Entity, v);
        for (const auto& ent : m_AdditionalEntities)
        {
            if (const EntityTransformBaseline* base = FindMultiEditBaseline(ent))
            {
                const Vector3 p = base->localPos + delta;
                ApplyLocalPositionToEntity(ent, Rendering::Vector3(p.x, p.y, p.z));
            }
        }
    }

    void ApplyLocalRotationToEntity(ECS::EntityHandle entity, const Rendering::Vector3& v)
    {
        ECS::Entity e(m_World, entity);
        if (!e.IsValid())
            return;

        if (const Transform* t = e.Get<Transform>())
        {
            Mathematics::WorldPosition p = t->GetPosition();
            Vector3 s = t->GetScale();
            Quaternion newRot = QuaternionFromEulerXYZDegrees(v.x, v.y, v.z);
            Transform updated = Transform::FromTRS(p, newRot, s);
            m_World->AddComponentImmediate(entity, updated);

            Vector3 parentPos;
            Quaternion parentRot;
            Vector3 parentScale{1.0f, 1.0f, 1.0f};
            GetParentWorldTRS(m_World, entity, parentPos, parentRot, parentScale);
            TRS worldTrs = CombineTRS(TRS{parentPos, parentRot, parentScale},
                                      TRS{p - Mathematics::WorldPosition{}, newRot, s});
            (void)Editor::TeleportPhysicsBody(m_World, entity, worldTrs.position, worldTrs.rotation);
        }
    }

    void ApplyLocalRotationEulerDegrees(const Rendering::Vector3& v)
    {
        EnsureMultiEditBaseline();
        const Quaternion primaryTarget = QuaternionFromEulerXYZDegrees(v.x, v.y, v.z);
        const Quaternion delta = (primaryTarget * m_MultiEditBaseline.primary.localRot.Conjugated()).Normalized();
        ApplyLocalRotationToEntity(m_Entity, v);
        for (const auto& ent : m_AdditionalEntities)
        {
            if (const EntityTransformBaseline* base = FindMultiEditBaseline(ent))
            {
                const Quaternion r = (delta * base->localRot).Normalized();
                float32 xDeg = 0.0f, yDeg = 0.0f, zDeg = 0.0f;
                EulerXYZDegreesFromQuaternion(r, xDeg, yDeg, zDeg);
                ApplyLocalRotationToEntity(ent, Rendering::Vector3(xDeg, yDeg, zDeg));
            }
        }
    }

    void ApplyLocalScaleToEntity(ECS::EntityHandle entity, const Rendering::Vector3& v)
    {
        ECS::Entity e(m_World, entity);
        if (!e.IsValid())
            return;

        if (const Transform* t = e.Get<Transform>())
        {
            Mathematics::WorldPosition p = t->GetPosition();
            Quaternion r = t->GetRotation();
            Vector3 newScale(v.x, v.y, v.z);
            Transform updated = Transform::FromTRS(p, r, newScale);
            m_World->AddComponentImmediate(entity, updated);
        }
    }

    void ApplyLocalScale(const Rendering::Vector3& v)
    {
        EnsureMultiEditBaseline();
        const Vector3 primaryTarget(v.x, v.y, v.z);
        const Vector3 primaryBase = m_MultiEditBaseline.primary.localScale;
        const Vector3 ratio(
            (primaryBase.x != 0.0f) ? (primaryTarget.x / primaryBase.x) : 1.0f,
            (primaryBase.y != 0.0f) ? (primaryTarget.y / primaryBase.y) : 1.0f,
            (primaryBase.z != 0.0f) ? (primaryTarget.z / primaryBase.z) : 1.0f);
        ApplyLocalScaleToEntity(m_Entity, v);
        for (const auto& ent : m_AdditionalEntities)
        {
            if (const EntityTransformBaseline* base = FindMultiEditBaseline(ent))
            {
                const Vector3 s(
                    base->localScale.x * ratio.x,
                    base->localScale.y * ratio.y,
                    base->localScale.z * ratio.z);
                ApplyLocalScaleToEntity(ent, Rendering::Vector3(s.x, s.y, s.z));
            }
        }
    }

    void ApplyWorldPositionToEntity(ECS::EntityHandle entity, const Rendering::Vector3& v)
    {
        ECS::Entity e(m_World, entity);
        if (!e.IsValid())
            return;

        if (const Transform* t = e.Get<Transform>())
        {
            Vector3 parentPos;
            Quaternion parentRot;
            Vector3 parentScale{1.0f, 1.0f, 1.0f};
            GetParentWorldTRS(m_World, entity, parentPos, parentRot, parentScale);

            Vector3 newWorldPos(v.x, v.y, v.z);
            Vector3 newLocalPos = ComputeLocalPositionFromWorld(newWorldPos, parentPos, parentRot, parentScale);

            Quaternion r = t->GetRotation();
            Vector3 s = t->GetScale();
            Transform updated = Transform::FromTRS(Mathematics::WorldPosition::FromVector3(newLocalPos), r, s);
            m_World->AddComponentImmediate(entity, updated);

            TRS worldTrs = ComputeWorldTRSForEntity(m_World, entity);
            Quaternion worldRot = worldTrs.rotation;
            (void)Editor::TeleportPhysicsBody(m_World, entity, newWorldPos, worldRot);
        }
    }

    void ApplyWorldPosition(const Rendering::Vector3& v)
    {
        EnsureMultiEditBaseline();
        const Vector3 primaryTarget(v.x, v.y, v.z);
        const Vector3 delta = primaryTarget - m_MultiEditBaseline.primary.worldPos;
        ApplyWorldPositionToEntity(m_Entity, v);
        for (const auto& ent : m_AdditionalEntities)
        {
            if (const EntityTransformBaseline* base = FindMultiEditBaseline(ent))
            {
                const Vector3 p = base->worldPos + delta;
                ApplyWorldPositionToEntity(ent, Rendering::Vector3(p.x, p.y, p.z));
            }
        }
    }

    void OnLocalPositionChanging(const Rendering::Vector3& v)
    {
        PreviewEdit("Transform Position", [this, v]() { ApplyLocalPosition(v); });
    }
    void OnLocalPositionChanged(const Rendering::Vector3& v)
    {
        CommitEdit("Transform Position", [this, v]() { ApplyLocalPosition(v); });
    }

    void OnLocalRotationChanging(const Rendering::Vector3& v)
    {
        PreviewEdit("Transform Rotation", [this, v]() { ApplyLocalRotationEulerDegrees(v); });
    }
    void OnLocalRotationChanged(const Rendering::Vector3& v)
    {
        CommitEdit("Transform Rotation", [this, v]() { ApplyLocalRotationEulerDegrees(v); });
    }

    void OnLocalScaleChanging(const Rendering::Vector3& v)
    {
        Rendering::Vector3 value = ResolveLinkedLocalScaleValue(v);
        if (m_LocalScaleLinked && m_LocalScale)
        {
            m_LocalScale->SetValue(value);
        }
        PreviewEdit("Transform Scale", [this, value]() { ApplyLocalScale(value); });
        RememberLinkedLocalScaleValue(value);
    }
    void OnLocalScaleChanged(const Rendering::Vector3& v)
    {
        Rendering::Vector3 value = ResolveLinkedLocalScaleValue(v);
        if (m_LocalScaleLinked && m_LocalScale)
        {
            m_LocalScale->SetValue(value);
        }
        CommitEdit("Transform Scale", [this, value]() { ApplyLocalScale(value); });
        RememberLinkedLocalScaleValue(value);
    }

    Rendering::Vector3 GetCurrentLocalScaleValue() const
    {
        if (m_LocalScale)
        {
            return m_LocalScale->GetValue();
        }

        if (m_World && m_Entity.IsValid() && m_World->IsValid(m_Entity))
        {
            if (auto* t = m_World->GetComponent<Transform>(m_Entity))
            {
                Vector3 s = t->GetScale();
                return Rendering::Vector3(s.x, s.y, s.z);
            }
        }

        return Rendering::Vector3(1.0f, 1.0f, 1.0f);
    }

    Rendering::Vector3 ResolveLinkedLocalScaleValue(const Rendering::Vector3& v)
    {
        if (!m_LocalScaleLinked)
        {
            return v;
        }

        const Rendering::Vector3 previous = m_HasLocalScaleLinkedLastValue
            ? m_LocalScaleLinkedLastValue
            : GetCurrentLocalScaleValue();

        const float dx = std::fabs(v.x - previous.x);
        const float dy = std::fabs(v.y - previous.y);
        const float dz = std::fabs(v.z - previous.z);

        float linkedValue = v.x;
        if (dy >= dx && dy >= dz)
        {
            linkedValue = v.y;
        }
        else if (dz >= dx && dz >= dy)
        {
            linkedValue = v.z;
        }

        return Rendering::Vector3(linkedValue, linkedValue, linkedValue);
    }

    void RememberLinkedLocalScaleValue(const Rendering::Vector3& v)
    {
        if (!m_LocalScaleLinked)
        {
            return;
        }

        m_LocalScaleLinkedLastValue = v;
        m_HasLocalScaleLinkedLastValue = true;
    }

    void OnWorldPositionChanging(const Rendering::Vector3& v)
    {
        PreviewEdit("Transform World Position", [this, v]() { ApplyWorldPosition(v); });
    }
    void OnWorldPositionChanged(const Rendering::Vector3& v)
    {
        CommitEdit("Transform World Position", [this, v]() { ApplyWorldPosition(v); });
    }

    // Helper to set up drag-to-adjust on a label for a Vector3Field
    // Double-click on label (without dragging) resets the row to its default value
    void SetupLabelDrag(Label* label, Vector3Field* field,
                        std::function<void(const Rendering::Vector3&)> onChanging,
                        std::function<void(const Rendering::Vector3&)> onChanged)
    {
        if (!label || !field)
            return;

        {
            label->Style().Set(Style::Cursor, CursorStyle::ColResize);
        }

        // Double-click detected on MouseDown (second press) so we don't rely on MouseUp
        auto lastMouseDownTime = std::make_shared<std::chrono::steady_clock::time_point>();
        auto dragDidMove = std::make_shared<bool>(false);

        label->RegisterEventHandler(kEventMouseDown, [this, label, field, lastMouseDownTime, dragDidMove](UIEvent& e) {
            if (e.Button == 0) {
                auto now = std::chrono::steady_clock::now();
                if ((now - *lastMouseDownTime) < GameEngine::Platform::GetDoubleClickInterval()) {
                    // Second click completes a double-click: reset all three, don't start a drag
                    ResetRowToDefault(field);
                    *lastMouseDownTime = std::chrono::steady_clock::time_point{};
                    e.Stop();
                    return;
                }
                *lastMouseDownTime = now;
                *dragDidMove = false;
                m_DraggingLabel = label;
                m_DraggingVec3Field = field;
                m_DragStartX = e.X;
                m_DragStartVec3 = field->GetValue();
                e.Capture(label);
                e.Stop();
            }
        });

        label->RegisterEventHandler(kEventMouseMove, [this, label, field, onChanging, dragDidMove](UIEvent& e) {
            if (m_DraggingLabel && m_DraggingVec3Field == field) {
                float deltaX = e.X - m_DragStartX;
                if (!InspectorDrag::BeginLabelDragPastThreshold(label, *dragDidMove, deltaX))
                {
                    e.Stop();
                    return;
                }
                Rendering::Vector3 newValue(
                    m_DragStartVec3.x + deltaX * InspectorDrag::kInspectorDragFloatSensitivity,
                    m_DragStartVec3.y + deltaX * InspectorDrag::kInspectorDragFloatSensitivity,
                    m_DragStartVec3.z + deltaX * InspectorDrag::kInspectorDragFloatSensitivity);
                field->SetValue(newValue);
                if (onChanging) onChanging(newValue);
                e.Stop();
            }
        });

        label->RegisterEventHandler(kEventMouseUp, [this, field, onChanged, dragDidMove](UIEvent& e) {
            if (m_DraggingLabel && m_DraggingVec3Field == field && e.Button == 0) {
                m_DraggingLabel = nullptr;
                m_DraggingVec3Field = nullptr;
                if (*dragDidMove)
                {
                    if (onChanged) onChanged(field->GetValue());
                }
                e.Stop();
            }
        });

        label->RegisterEventHandler(kEventMouseCancel, [this, field, onChanged, dragDidMove](UIEvent& e)
                                    { CancelLabelDrag(e, field, *dragDidMove, onChanged); });
    }

    // Escape or another button during a row label drag: a drag that moved the row puts back its start
    // values, and onChanged commits them.
    void CancelLabelDrag(UIEvent& e, Vector3Field* field, bool dragDidMove,
                         const std::function<void(const Rendering::Vector3&)>& onChanged)
    {
        if (!m_DraggingLabel || m_DraggingVec3Field != field)
            return;
        m_DraggingLabel = nullptr;
        m_DraggingVec3Field = nullptr;
        if (dragDidMove)
        {
            field->SetValue(m_DragStartVec3);
            if (onChanged)
                onChanged(m_DragStartVec3);
        }
        e.Stop();
    }

    const char* TransformVectorRowLabelForMenu(Vector3Field* field) const
    {
        if (field == m_LocalPos)
            return "Local Position";
        if (field == m_LocalRot)
            return "Local Rotation";
        if (field == m_LocalScale)
            return "Local Scale";
        if (field == m_WorldPos)
            return "World Position";
        return "Transform";
    }

    // Drops the "value unchanged since last refresh" guards so the next refresh
    // repaints every row, including ones an edit changed indirectly (local ->
    // world position).
    void InvalidateFieldValueCache()
    {
        m_HasLastLocalPos = false;
        m_HasLastLocalRot = false;
        m_HasLastLocalScale = false;
        m_HasLastWorldPos = false;
    }

    // Reset is absolute for every selected entity: each lands on the row default.
    // Value edits instead offset the rest of the selection by the primary's delta,
    // which would leave a multi-selection scattered around the default.
    void ApplyRowValueToSelection(Vector3Field* field, const Rendering::Vector3& v)
    {
        auto applyOne = [this, field, &v](ECS::EntityHandle entity)
        {
            if (field == m_LocalPos)
                ApplyLocalPositionToEntity(entity, v);
            else if (field == m_LocalRot)
                ApplyLocalRotationToEntity(entity, v);
            else if (field == m_LocalScale)
                ApplyLocalScaleToEntity(entity, v);
            else if (field == m_WorldPos)
                ApplyWorldPositionToEntity(entity, v);
        };

        applyOne(m_Entity);
        for (const auto& ent : m_AdditionalEntities)
            applyOne(ent);
    }

    void ResetRowToDefault(Vector3Field* field)
    {
        if (!field || !m_World || !m_Entity.IsValid() || !m_World->IsValid(m_Entity))
            return;

        const Rendering::Vector3 def = field->GetDefaultValue();
        const std::string undoName = std::string("Reset ") + TransformVectorRowLabelForMenu(field);

        FinalizeActiveInteractiveEditIfAny();
        CommitEdit(undoName.c_str(), [this, field, def]() { ApplyRowValueToSelection(field, def); });

        if (field == m_LocalScale)
            RememberLinkedLocalScaleValue(def);

        InvalidateFieldValueCache();
        field->SetValue(def);
        RefreshFromWorld();
    }

    void ResetAllTransformValues()
    {
        if (!m_World || !m_Entity.IsValid() || !m_World->IsValid(m_Entity))
            return;
        if (!m_LocalPos || !m_LocalRot || !m_LocalScale)
            return;

        const Rendering::Vector3 pos = m_LocalPos->GetDefaultValue();
        const Rendering::Vector3 rot = m_LocalRot->GetDefaultValue();
        const Rendering::Vector3 scale = m_LocalScale->GetDefaultValue();

        FinalizeActiveInteractiveEditIfAny();
        CommitEdit("Reset Transform", [this, pos, rot, scale]() {
            ApplyAllTRSToEntity(m_Entity, pos, rot, scale);
            for (const auto& ent : m_AdditionalEntities)
                ApplyAllTRSToEntity(ent, pos, rot, scale);
        });

        RememberLinkedLocalScaleValue(scale);

        InvalidateFieldValueCache();
        m_LocalPos->SetValue(pos);
        m_LocalRot->SetValue(rot);
        m_LocalScale->SetValue(scale);
        RefreshFromWorld();
    }

    // The row's own menu entries: Copy, Paste and Reset of the whole row (X, Y, Z), and on Local Scale the
    // linked-scale toggle. Avoid '/' in labels: the menu splits paths on '/' into submenus.
    std::vector<ContextMenuManipulator::Item> RowMenuItems(
        Vector3Field* field, const std::function<void(const Rendering::Vector3&)>& onChanged)
    {
        const std::string rowName = TransformVectorRowLabelForMenu(field);
        std::vector<ContextMenuManipulator::Item> items = {
            {.Path = "Copy " + rowName + " (X, Y, Z)",
             .IconPath = EditorIcons::kCopy,
             .OnActivate = [field]() { CopyRow(*field); }},
            {.Path = "Paste " + rowName + " (X, Y, Z)",
             .IconPath = EditorIcons::kPaste,
             .OnActivate = [this, field, onChanged]() { PasteRow(*field, onChanged); },
             .State = &RowPasteState},
            {.Separator = true},
            {.Path = "Reset " + rowName + " (X, Y, Z)",
             .IconPath = EditorIcons::kReset,
             .OnActivate = [this, field]() { ResetRowToDefault(field); }},
        };
        if (field != m_LocalScale)
            return items;
        items.push_back({.Separator = true});
        items.push_back({.Path = "Linked Scale Values",
                         .IconPath = EditorIcons::kLink,
                         .OnActivate = [this]() { ToggleLinkedScale(); },
                         .State = [this]() { return ContextMenuManipulator::ItemState{.Checked = m_LocalScaleLinked}; }});
        return items;
    }

    // Paste is enabled while the clipboard holds three numbers. A read that can prompt the user (web) is not
    // made to peek: Paste stays enabled, and its own read does nothing on text that does not parse.
    static ContextMenuManipulator::ItemState RowPasteState()
    {
        if (Platform::ClipboardReadCanPromptUser())
            return {};
        Rendering::Vector3 parsed;
        return {.Enabled = TryParseTransformRowClipboardText(Platform::GetClipboardText(), parsed)};
    }

    static void CopyRow(const Vector3Field& field)
    {
        Platform::SetClipboardText(TransformRowClipboardText(field.GetValue()).c_str());
    }

    void PasteRow(Vector3Field& field, const std::function<void(const Rendering::Vector3&)>& onChanged)
    {
        Rendering::Vector3 parsed;
        if (!TryParseTransformRowClipboardText(Platform::GetClipboardText(), parsed))
            return;
        FinalizeActiveInteractiveEditIfAny();
        field.SetValue(parsed);
        if (onChanged)
            onChanged(parsed);
    }

    void ToggleLinkedScale()
    {
        m_LocalScaleLinked = !m_LocalScaleLinked;
        m_HasLocalScaleLinkedLastValue = m_LocalScaleLinked;
        if (m_LocalScaleLinked)
            m_LocalScaleLinkedLastValue = GetCurrentLocalScaleValue();
    }

    void ApplyAllTRSToEntity(ECS::EntityHandle entity,
                             const Rendering::Vector3& pos,
                             const Rendering::Vector3& rotEulerDeg,
                             const Rendering::Vector3& scale)
    {
        ECS::Entity e(m_World, entity);
        if (!e.IsValid())
            return;

        if (const Transform* t = e.Get<Transform>())
        {
            (void)t;
            Vector3 newPos(pos.x, pos.y, pos.z);
            Quaternion newRot = QuaternionFromEulerXYZDegrees(rotEulerDeg.x, rotEulerDeg.y, rotEulerDeg.z);
            Vector3 newScale(scale.x, scale.y, scale.z);
            Transform updated = Transform::FromTRS(Mathematics::WorldPosition::FromVector3(newPos), newRot, newScale);
            m_World->AddComponentImmediate(entity, updated);

            Vector3 parentPos;
            Quaternion parentRot;
            Vector3 parentScale{1.0f, 1.0f, 1.0f};
            GetParentWorldTRS(m_World, entity, parentPos, parentRot, parentScale);
            TRS worldTrs = CombineTRS(TRS{parentPos, parentRot, parentScale}, TRS{newPos, newRot, newScale});
            (void)Editor::TeleportPhysicsBody(m_World, entity, worldTrs.position, worldTrs.rotation);
        }
    }

    void ApplyAllTRS(const Rendering::Vector3& pos,
                     const Rendering::Vector3& rotEulerDeg,
                     const Rendering::Vector3& scale)
    {
        EnsureMultiEditBaseline();
        const Vector3 primaryPos(pos.x, pos.y, pos.z);
        const Vector3 posDelta = primaryPos - m_MultiEditBaseline.primary.localPos;
        const Quaternion primaryRot = QuaternionFromEulerXYZDegrees(rotEulerDeg.x, rotEulerDeg.y, rotEulerDeg.z);
        const Quaternion rotDelta = (primaryRot * m_MultiEditBaseline.primary.localRot.Conjugated()).Normalized();
        const Vector3 primaryScale(scale.x, scale.y, scale.z);
        const Vector3 primaryBaseScale = m_MultiEditBaseline.primary.localScale;
        const Vector3 scaleRatio(
            (primaryBaseScale.x != 0.0f) ? (primaryScale.x / primaryBaseScale.x) : 1.0f,
            (primaryBaseScale.y != 0.0f) ? (primaryScale.y / primaryBaseScale.y) : 1.0f,
            (primaryBaseScale.z != 0.0f) ? (primaryScale.z / primaryBaseScale.z) : 1.0f);

        ApplyAllTRSToEntity(m_Entity, pos, rotEulerDeg, scale);
        for (const auto& ent : m_AdditionalEntities)
        {
            if (const EntityTransformBaseline* base = FindMultiEditBaseline(ent))
            {
                const Vector3 p = base->localPos + posDelta;
                const Quaternion r = (rotDelta * base->localRot).Normalized();
                const Vector3 s(
                    base->localScale.x * scaleRatio.x,
                    base->localScale.y * scaleRatio.y,
                    base->localScale.z * scaleRatio.z);
                float32 xDeg = 0.0f, yDeg = 0.0f, zDeg = 0.0f;
                EulerXYZDegreesFromQuaternion(r, xDeg, yDeg, zDeg);
                ApplyAllTRSToEntity(ent,
                                    Rendering::Vector3(p.x, p.y, p.z),
                                    Rendering::Vector3(xDeg, yDeg, zDeg),
                                    Rendering::Vector3(s.x, s.y, s.z));
            }
        }
    }

    struct EntityTransformBaseline
    {
        ECS::EntityHandle entity{};
        Vector3 localPos{};
        Quaternion localRot{};
        Vector3 localScale{1.0f, 1.0f, 1.0f};
        Vector3 worldPos{};
    };

    void ClearMultiEditBaseline()
    {
        m_MultiEditBaseline.valid = false;
        m_MultiEditBaseline.additional.clear();
    }

    void EnsureMultiEditBaseline()
    {
        if (m_MultiEditBaseline.valid)
        {
            return;
        }

        m_MultiEditBaseline.valid = true;
        m_MultiEditBaseline.primary = CaptureTransformBaseline(m_Entity);
        m_MultiEditBaseline.additional.clear();
        m_MultiEditBaseline.additional.reserve(m_AdditionalEntities.size());
        for (const auto& ent : m_AdditionalEntities)
        {
            if (!ent.IsValid() || !m_World || !m_World->IsValid(ent))
            {
                continue;
            }
            if (!m_World->GetComponent<Transform>(ent))
            {
                continue;
            }
            m_MultiEditBaseline.additional.push_back(CaptureTransformBaseline(ent));
        }
    }

    EntityTransformBaseline CaptureTransformBaseline(ECS::EntityHandle entity) const
    {
        EntityTransformBaseline out{};
        out.entity = entity;
        if (!m_World || !entity.IsValid() || !m_World->IsValid(entity))
        {
            return out;
        }

        if (const Transform* t = m_World->GetComponent<Transform>(entity))
        {
            out.localPos = t->GetPosition() - Mathematics::WorldPosition{};
            out.localRot = t->GetRotation();
            out.localScale = t->GetScale();
        }
        out.worldPos = ComputeWorldTRSForEntity(m_World, entity).position;
        return out;
    }

    const EntityTransformBaseline* FindMultiEditBaseline(ECS::EntityHandle entity) const
    {
        for (const auto& base : m_MultiEditBaseline.additional)
        {
            if (base.entity == entity)
            {
                return &base;
            }
        }
        return nullptr;
    }

    void ShowSectionContextMenu(float x, float y)
    {
        if (!m_Window)
            return;
        if (!m_World || !m_Entity.IsValid() || !m_World->IsValid(m_Entity))
            return;

        if (!m_SectionContextMenu)
        {
            m_SectionContextMenu = CreateContextMenu();
            if (!m_SectionContextMenu)
                return;
            m_SectionContextMenu->SetCommandHandler([this](uint32_t cmd) { OnContextMenuCommand(cmd); });
        }

        // As in RowPasteState: a peek-only read risks
        // a permission prompt on web for every right-click, so skip it there.
        bool canPaste = true;
        if (!Platform::ClipboardReadCanPromptUser())
        {
            std::string clip = Platform::GetClipboardText();
            Rendering::Vector3 dPos, dRot, dScale;
            canPaste = TryParseAllTRSFromClipboard(clip, dPos, dRot, dScale);
        }

        m_SectionContextMenu->Clear();
        ContextMenuBuilder builder;
        // Avoid '/' in labels — ContextMenuBuilder splits on '/' and would create accidental nested submenus.
        builder.AddItem("Copy All Transform Values",
                        kCmdCopyAll,
                        MenuItemFlag_None,
                        0,
                        EditorIcons::kCopy);
        builder.AddItem("Paste All Transform Values",
                        kCmdPasteAll,
                        canPaste ? MenuItemFlag_None : MenuItemFlag_Disabled,
                        10,
                        EditorIcons::kPaste);
        builder.AddItem("---reset", 0, MenuItemFlag_None, 14);
        builder.AddItem("Reset All Transform Values",
                        kCmdResetAll,
                        MenuItemFlag_None,
                        15,
                        EditorIcons::kReset);
        builder.Build(m_SectionContextMenu.get());
        m_SectionContextMenu->Show(m_Window, static_cast<int>(x), static_cast<int>(y));
    }

    void OnContextMenuCommand(uint32_t cmd)
    {
        if (cmd == kCmdResetAll)
        {
            ResetAllTransformValues();
        }
        else if (cmd == kCmdCopyAll)
        {
            if (!m_World || !m_Entity.IsValid() || !m_World->IsValid(m_Entity))
                return;
            auto* t = m_World->GetComponent<Transform>(m_Entity);
            if (!t)
                return;
            const Mathematics::WorldPosition p = t->GetPosition();
            Vector3 s = t->GetScale();
            float xDeg = 0.0f, yDeg = 0.0f, zDeg = 0.0f;
            EulerXYZDegreesFromQuaternion(t->GetRotation(), xDeg, yDeg, zDeg);
            TRS worldTrs = ComputeWorldTRSForEntity(m_World, m_Entity);
            Vector3 wp = worldTrs.position;
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                          "Position: %g, %g, %g\n"
                          "Rotation: %g, %g, %g\n"
                          "Scale: %g, %g, %g\n"
                          "World Position: %g, %g, %g",
                          p.X, p.Y, p.Z,
                          xDeg, yDeg, zDeg,
                          s.x, s.y, s.z,
                          wp.x, wp.y, wp.z);
            Platform::SetClipboardText(buf);
        }
        else if (cmd == kCmdPasteAll)
        {
            std::string clip = Platform::GetClipboardText();
            Rendering::Vector3 pos, rot, scale;
            if (TryParseAllTRSFromClipboard(clip, pos, rot, scale))
            {
                FinalizeActiveInteractiveEditIfAny();
                CommitEdit("Paste Transform", [this, pos, rot, scale]() {
                    ApplyAllTRS(pos, rot, scale);
                });

                // Force UI refresh so all four rows reflect the pasted values immediately,
                // bypassing the focus/cache guards in the notification path.
                InvalidateFieldValueCache();
                if (m_LocalPos)   m_LocalPos->SetValue(pos);
                if (m_LocalRot)   m_LocalRot->SetValue(rot);
                if (m_LocalScale) m_LocalScale->SetValue(scale);
                RefreshFromWorld();
            }
        }
    }

    static constexpr uint32_t kCmdCopyAll  = 3;
    static constexpr uint32_t kCmdPasteAll = 4;
    static constexpr uint32_t kCmdResetAll = 7;

private:
    ECS::World* m_World = nullptr;
    ECS::EntityHandle m_Entity{};
    std::vector<ECS::EntityHandle> m_AdditionalEntities;
    Editor::UndoRedoService* m_Undo = nullptr;
    Editor::EditorChangeNotifications* m_Changes = nullptr;
    Editor::EditorChangeNotifications::SubscriptionToken m_Sub{};
    Platform::Window* m_Window = nullptr;

    Vector3Field* m_LocalPos = nullptr;
    Vector3Field* m_LocalRot = nullptr;
    Vector3Field* m_LocalScale = nullptr;
    Vector3Field* m_WorldPos = nullptr;

    Editor::UndoRedoService::InteractiveEdit m_ActiveEdit{};

    // PERF: avoid updating fields unless values actually changed.
    bool m_HasLastLocalPos = false;
    bool m_HasLastLocalRot = false;
    bool m_HasLastLocalScale = false;
    bool m_HasLastWorldPos = false;
    Rendering::Vector3 m_LastLocalPos{};
    Rendering::Vector3 m_LastLocalRot{};
    Rendering::Vector3 m_LastLocalScale{};
    Rendering::Vector3 m_LastWorldPos{};

    // Independent from viewport frame rate: live numeric previews are capped
    // at 30 Hz while transform propagation and gizmo rendering remain uncapped.
    std::chrono::steady_clock::time_point m_LastPreviewRefresh{};

    // Drag-to-adjust state for label dragging
    Label* m_DraggingLabel = nullptr;
    Vector3Field* m_DraggingVec3Field = nullptr;
    float m_DragStartX = 0.0f;
    Rendering::Vector3 m_DragStartVec3{};

    // The header menu: copy, paste and reset of all values.
    std::unique_ptr<INativeContextMenu> m_SectionContextMenu;

    bool m_LocalScaleLinked = false;
    bool m_HasLocalScaleLinkedLastValue = false;
    Rendering::Vector3 m_LocalScaleLinkedLastValue{1.0f, 1.0f, 1.0f};

    struct MultiEditBaseline
    {
        bool valid = false;
        EntityTransformBaseline primary{};
        std::vector<EntityTransformBaseline> additional;
    } m_MultiEditBaseline{};

    bool m_TransformWideLayout = false;
    bool m_TransformWideLayoutInitialized = false;
};

} // anonymous namespace

std::string TransformRowClipboardText(const Vector3& value)
{
    // Six significant digits: short text that Paste reads back.
    constexpr int kClipboardSignificantDigits = 6;
    const std::array<std::string, 3> components = {FormatFloat(value.x, kClipboardSignificantDigits),
                                                   FormatFloat(value.y, kClipboardSignificantDigits),
                                                   FormatFloat(value.z, kClipboardSignificantDigits)};
    return Join(components, ", ");
}

bool TryParseTransformRowClipboardText(const std::string& text, Vector3& out)
{
    std::string spaced = text;
    for (char& c : spaced)
    {
        if (c == ',' || c == ';')
            c = ' ';
    }
    std::string_view rest = spaced;
    Vector3 parsed;
    constexpr size_t kAxisCount = 3;
    for (size_t axis = 0; axis < kAxisCount; ++axis)
    {
        size_t consumed = 0;
        const std::optional<float> value = ParseLeadingFloat(rest, &consumed);
        if (!value)
            return false;
        parsed[axis] = *value;
        rest.remove_prefix(consumed);
    }
    out = parsed;
    return true;
}

void RegisterTransformInspector()
{
    InspectorFn transformInspector = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
        {
            return;
        }

        auto section = std::make_unique<TransformInspectorSection>(ctx);
        ctx.Parent->AddChild(std::move(section));
    };

    InspectorRegistry::Get().RegisterComponentInspector<Transform>(std::move(transformInspector));
}

} // namespace GameEngine
