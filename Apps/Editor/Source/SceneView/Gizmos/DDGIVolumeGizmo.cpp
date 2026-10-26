#include "SceneView/Gizmos/DDGIVolumeGizmo.h"

#include <algorithm>
#include <cmath>

#include "Components/Rendering/DDGIVolume.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/Components.h"
#include "ECS/ECS.h"
#include "ECS/Query.h"
#include "Engine/Rendering/DDGIProbeFeature.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/Vector3.h"
#include "Types/Color.h"

namespace GameEngine::Editor::SceneTools
{

using GameEngine::Components::DDGIVolume;
using GameEngine::Components::WorldTransform;
using GameEngine::Mathematics::Vector3;

namespace
{

// Visualization-only cap: at Divisions=32 on a cubical volume the grid
// is up to 32^3 = 32768 probes, and drawing one marker per probe every frame
// while selected would be a real editor hitch. Subsample with a stride
// instead of drawing every probe past this cap — an authoring preview, not a
// claim about the actual traced probe count (the inspector's Divisions
// field is that source of truth).
constexpr int kMaxDrawnProbeMarkers = 4096;
constexpr float kProbeMarkerHalfExtent = 0.06f;

// A small diamond-in-a-square icon — visually distinct from
// ReflectionProbeGizmo's circular icon at a glance.
void DrawVolumeIcon(GizmoRenderContext& ctx, const Vector3& pos, const Vector3& right, const Vector3& up,
                    float radius, const Color& color, float thickness)
{
    GizmoLineBatch batch(ctx, color, thickness);

    auto point = [&](float x, float y)
    {
        return pos + right * x * radius + up * y * radius;
    };

    const Vector3 corners[4] = {
        point(-0.8f, -0.8f),
        point(0.8f, -0.8f),
        point(0.8f, 0.8f),
        point(-0.8f, 0.8f)};
    for (int i = 0; i < 4; ++i)
        batch.AddLine(corners[i], corners[(i + 1) % 4]);

    const Vector3 diamond[4] = {
        point(0.0f, -0.9f),
        point(0.9f, 0.0f),
        point(0.0f, 0.9f),
        point(-0.9f, 0.0f)};
    for (int i = 0; i < 4; ++i)
        batch.AddLine(diamond[i], diamond[(i + 1) % 4]);
}

void DrawProbeMarker(GizmoLineBatch& batch, const Vector3& pos, float halfExtent)
{
    batch.AddLine(Vector3(pos.x - halfExtent, pos.y, pos.z), Vector3(pos.x + halfExtent, pos.y, pos.z));
    batch.AddLine(Vector3(pos.x, pos.y - halfExtent, pos.z), Vector3(pos.x, pos.y + halfExtent, pos.z));
    batch.AddLine(Vector3(pos.x, pos.y, pos.z - halfExtent), Vector3(pos.x, pos.y, pos.z + halfExtent));
}

} // namespace

void DDGIVolumeGizmo::SetSelection(const std::vector<GameEngine::ECS::EntityHandle>& entities)
{
    m_SelectedEntities = entities;
}

void DDGIVolumeGizmo::SetHovered(GameEngine::ECS::EntityHandle entity)
{
    m_HoveredEntity = entity;
}

void DDGIVolumeGizmo::Render(GizmoRenderContext& context)
{
    GameEngine::ECS::World* world = context.GetWorld();
    if (!world)
        return;

    const Vector3* camPosPtr = context.GetCameraWorldPosition();
    if (!camPosPtr)
        return;
    const Vector3& camPos = *camPosPtr;
    const float orthoHeight = (context.HasOrthoHeight() && context.GetOrthoHeight() > 0.0f)
                                  ? context.GetOrthoHeight()
                                  : 0.0f;

    world->Query<
             GameEngine::ECS::Read<DDGIVolume>,
             GameEngine::ECS::Read<WorldTransform>>()
        .Each(
            [&](GameEngine::ECS::EntityHandle e, const DDGIVolume& volume, const WorldTransform& xf)
            {
                const bool isHovered = m_HoveredEntity.IsValid() && e == m_HoveredEntity;
                const bool isSelected =
                    std::find(m_SelectedEntities.begin(), m_SelectedEntities.end(), e) !=
                    m_SelectedEntities.end();
                const bool emphasize = isHovered || isSelected;

                // Translation places the volume; its scale sizes it (unit cube
                // × transform scale — DDGIVolume.h's doc). v1 ignores rotation
                // only, so this is still a plain world-axis-aligned box and no
                // oriented-box math is needed.
                const Vector3 pos = xf.GetPosition();

                Vector3 right;
                Vector3 up;
                BuildBillboardBasis(pos, camPos, right, up);
                const float iconRadius = ComputeGizmoIconWorldRadius(pos, camPos, orthoHeight);
                const Color iconColor(emphasize ? 1.0f : 0.85f, emphasize ? 0.65f : 0.45f, 0.15f,
                                      emphasize ? 1.0f : 0.78f);
                DrawVolumeIcon(context, pos, right, up, iconRadius, iconColor, emphasize ? 2.0f : 1.35f);

                // Selection must not force the lattice on. The only way to reach the
                // ShowProbes checkbox is to select the volume, so ORing selection in
                // here made the checkbox a no-op at exactly the moment it is used.
                // Selection still emphasises the icon and draws the bounds box below.
                const bool drawProbes = volume.ShowProbes;

                if (!emphasize && !drawProbes)
                    return;

                // Same derivation the extraction system feeds the GPU, so the
                // drawn box is exactly the sampled box.
                Vector3 halfExtents = Engine::Renderer::DDGIProbeFeature::ComputeVolumeHalfExtents(
                    Mathematics::Matrix4x4::FromColumnMajor(xf.matrix));

                // A following volume is not where its transform says it is, so
                // draw it where DDGIVolumeSystem::ApplyCameraFit puts it. The
                // gizmo cannot see that system's hysteresis state, so this
                // tracks the camera continuously while the runtime grid moves
                // in steps: the box is right whenever the camera is at rest,
                // which is when anyone reads it.
                Vector3 centre = pos;
                if (volume.Fit == Components::DDGIVolumeFit::FollowCamera)
                {
                    // A followed volume ignores the transform entirely: its size
                    // comes from FollowRange and its centre from the camera. Both
                    // derivations must match DDGIVolumeSystem::ApplyCameraFit, or
                    // the drawn box is not the sampled one.
                    const float range = std::max(volume.FollowRange, 0.1f);
                    const float heightFraction = std::clamp(volume.FollowHeightFraction, 0.05f, 4.0f);
                    halfExtents = Vector3(range, range * heightFraction, range);
                    const Vector3 gridSizeWS = halfExtents * 2.0f;
                    centre = Engine::Renderer::DDGIProbeFeature::SnapCentreToProbeGrid(camPos, gridSizeWS,
                                                                                       volume.Divisions);
                }
                if (emphasize)
                {
                    const Color boxColor(1.0f, 0.72f, 0.2f, isSelected ? 0.85f : 0.55f);
                    context.DrawWireBox(centre, halfExtents, boxColor,
                                        isSelected ? 1.6f : 1.2f);
                }

                if (!drawProbes)
                    return;

                const Vector3 gridSizeWS = halfExtents * 2.0f;
                int32_t probeCount[3];
                Engine::Renderer::DDGIProbeFeature::ComputeProbeGridLayout(gridSizeWS, volume.Divisions,
                                                                           probeCount);
                const int32_t probeTotal = probeCount[0] * probeCount[1] * probeCount[2];
                const int32_t stride = std::max(1, probeTotal / kMaxDrawnProbeMarkers);

                const Color markerColor(0.35f, 0.85f, 1.0f, 0.7f);
                GizmoLineBatch batch(context, markerColor, 1.0f);
                const Vector3 gridMin = centre - halfExtents;
                for (int32_t idx = 0; idx < probeTotal; idx += stride)
                {
                    const int32_t x = idx % probeCount[0];
                    const int32_t y = (idx / probeCount[0]) % probeCount[1];
                    const int32_t z = idx / (probeCount[0] * probeCount[1]);
                    const Vector3 t(
                        probeCount[0] > 1 ? static_cast<float>(x) / static_cast<float>(probeCount[0] - 1)
                                          : 0.5f,
                        probeCount[1] > 1 ? static_cast<float>(y) / static_cast<float>(probeCount[1] - 1)
                                          : 0.5f,
                        probeCount[2] > 1 ? static_cast<float>(z) / static_cast<float>(probeCount[2] - 1)
                                          : 0.5f);
                    const Vector3 probePos(gridMin.x + t.x * gridSizeWS.x, gridMin.y + t.y * gridSizeWS.y,
                                           gridMin.z + t.z * gridSizeWS.z);
                    DrawProbeMarker(batch, probePos, kProbeMarkerHalfExtent);
                }
            });
}

} // namespace GameEngine::Editor::SceneTools
