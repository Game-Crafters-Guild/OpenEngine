#pragma once

#include "SceneView/SceneViewGizmos.h"
#include "SceneView/SplineTool/SplineTool.h"

#include "ECS/Entity.h"
#include "SceneView/SplineTool/SplineDrapePolylines.h"
#include "SceneView/SplineTool/SplineJunctionMarkers.h"
#include "Spline/SplineTypes.h"
#include "SplineRecipes/SplineSurfaceConform.h"
#include "Types/FlatMap.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace GameEngine::Components { struct SplineComponent; struct WorldTransform; }

namespace GameEngine::Editor::SceneTools
{

// Persistent gizmo that renders CatmullRom curves for ALL SplineComponent
// entities in the scene. Unlike SplineGizmo (which only renders the active
// tool stroke), this gizmo keeps splines visible after creation.
class SplineSceneGizmo : public IGizmo
{
public:
    explicit SplineSceneGizmo(ECS::World& world) : m_State(AcquireSplineInteractionState(world)) {}
    SplineInteractionState& State() const { return *m_State; }

    void SetSelectionQuery(std::function<ECS::EntityHandle()> query)
    {
        m_SelectionQuery = std::move(query);
    }

    void SetIsEntitySelectedQuery(std::function<bool(ECS::EntityHandle)> query)
    {
        m_IsEntitySelectedQuery = std::move(query);
    }

    /// Picks the junction marker under the pointer, whatever tool is active.
    void UpdateMarkerHover(const GizmoRay& ray) { m_JunctionMarkers.UpdateHover(m_State->World(), ray); }
    /// The hovered junction marker's text; empty for none.
    const std::string& GetMarkerHoverText() const { return m_JunctionMarkers.GetHoverText(); }

    // IGizmo
    void Render(GizmoRenderContext& context) override;

    /// Test seam: the reads of the meshes under a draped spline since construction.
    uint64_t GetSceneDigestRefreshCountForTesting() const { return m_SceneDigestRefreshCount; }

private:
    std::shared_ptr<SplineInteractionState> m_State;
    SplineJunctionMarkers m_JunctionMarkers;
    struct PassiveLineCache
    {
        uint32_t DataIndex = 0;
        uint32_t Generation = 0;
        uint64_t DataVersion = 0;
        float Matrix[16] = {};
        std::vector<Mathematics::Vector3> Vertices;
        uint64_t LastSeen = 0;
    };

    // Draped display polylines per spline (centerline when draping is on;
    // ±width envelope edges for the selected spline), built by conform
    // raycasts and rebuilt only when the spline data, its version, the entity
    // transform, the edge requirement, or the ground it was draped onto
    // changes. Stored in world space — the drape is view-independent; only the
    // camera-facing thick-line expansion runs per frame.
    struct DrapeCache
    {
        uint32_t DataIndex = 0;
        uint32_t Generation = 0;
        uint64_t DataVersion = 0;
        float Matrix[16] = {};
        bool HasEdges = false;
        // The ground these polylines were draped onto (ConformSurfaceRevision).
        // The drape is MEASURED, so a bake, a sculpt or a moved terrain
        // invalidates it with no spline-side input changing — and the displayed
        // centerline must not disagree with where the placement controllers put
        // the pieces, which observe the same revision.
        uint64_t SurfaceRevision = 0;
        // The run's world bounds at the keys above.
        Mathematics::AABB RunBounds = Mathematics::AABB::Empty();
        // The meshes under the run the drape can land on. A mesh that arrives, moves or leaves
        // there changes SceneDigest.Value, and the drape is rebuilt when it differs from SceneRevision.
        SplineRecipes::SceneConformDigest SceneDigest;
        uint64_t SceneRevision = 0;
        // A ray crossed a mesh whose picking data was not ready, so the drape missed that mesh.
        // The entry is rebuilt on the next frame until no ray waits.
        bool OverPendingMesh = false;
        SplineDrapedPolylines Polylines;
        // Each knot's draped height (the surface under it plus the drape's lift), NaN where the
        // conform ray missed: the foot of the knot's stem.
        std::vector<float> KnotDrapedY;
        uint64_t LastSeen = 0;
    };

    std::function<ECS::EntityHandle()> m_SelectionQuery;
    std::function<bool(ECS::EntityHandle)> m_IsEntitySelectedQuery;
    mutable uint64_t m_CacheEpoch = 0;
    uint64_t m_SceneDigestRefreshCount = 0;
    mutable FlatMap<uint32_t, PassiveLineCache> m_PassiveLineCache;
    mutable FlatMap<uint32_t, DrapeCache> m_DrapeCache;

    // What one Render pass reads for all its drapes, once and only when a spline is draped.
    struct DrapePassState;
    // Refreshes (and rebuilds when dirty) the drape cache entry of one spline; wantEdges adds the
    // ±width envelope edges. The reference is valid until the next call or the end of Render,
    // because an insert or an eviction moves the entries.
    const DrapeCache& RefreshDrapeCache(ECS::World& world,
                                        const SplineECS::SplineService& splineService,
                                        DrapePassState& pass,
                                        ECS::EntityHandle e,
                                        const Components::SplineComponent& comp,
                                        const Spline::SplineData& data,
                                        const Components::WorldTransform& xf,
                                        const Mathematics::Matrix4x4& worldM,
                                        bool wantEdges);

    // The vertex lists one Render fills and flushes. Render clears them and keeps their
    // capacity, so a frame that draws what the frame before drew allocates nothing.
    struct DrawBatches
    {
        std::vector<Mathematics::Vector3> Centerline;
        std::vector<Mathematics::Vector3> Knot;
        std::vector<Mathematics::Vector3> Handle;
        std::vector<Mathematics::Vector3> Selected;
        std::vector<Mathematics::Vector3> HoverKnot;
        std::vector<Mathematics::Vector3> HoverHandle;
        std::vector<Mathematics::Vector3> KnotStems;
        std::vector<Mathematics::Vector3> Envelope;
        std::vector<Mathematics::Vector3> EnvelopeFill;
        // The line pairs of one passive draped centerline.
        std::vector<Mathematics::Vector3> PassiveDraped;

        void Clear()
        {
            for (std::vector<Mathematics::Vector3>* batch :
                 {&Centerline, &Knot, &Handle, &Selected, &HoverKnot, &HoverHandle, &KnotStems, &Envelope,
                  &EnvelopeFill, &PassiveDraped})
                batch->clear();
        }
    };
    DrawBatches m_Batches;
    // One spline's control points in world space, rebuilt for each spline that shows its controls.
    std::vector<Spline::SplineControlPoint> m_WorldPoints;
};

} // namespace GameEngine::Editor::SceneTools
