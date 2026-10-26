#include "ECSModules/Rendering/Systems/ReflectionProbeSystem.h"

#include "Components/Rendering/Camera.h"
#include "Components/Rendering/ReflectionProbe.h"
#include "Components/Transform.h"
#include "ECS/Query.h"
#include "Engine/Rendering/Camera.h"
#include "Engine/Rendering/ImageBasedLightingFeature.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SceneReflectionProbeEnvironmentSource.h"
#include "Engine/Rendering/SkyEnvironmentSource.h"

#include "Logger/Logger.h"
#include "Mathematics/Interpolation.h"
#include "Mathematics/Vector3.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>

namespace GameEngine { namespace Engine::Renderer {

namespace
{
using Mathematics::Vector3;

constexpr float kReflectionProbeDegToRad = 0.01745329251994329577f;
constexpr float kMinProbeExtent = 0.0001f;

SceneReflectionProbeUpdateMode ToSceneUpdateMode(Components::ReflectionProbeUpdateMode mode)
{
    switch (mode)
    {
    case Components::ReflectionProbeUpdateMode::Once:
        return SceneReflectionProbeUpdateMode::Once;
    case Components::ReflectionProbeUpdateMode::Realtime:
    default:
        return SceneReflectionProbeUpdateMode::Realtime;
    }
}

struct ProbeCandidate
{
    Components::ReflectionProbe Probe{};
    uint32 Priority = 0;
    float NormalizedDistanceSq = 0.0f;
    float Weight = 1.0f;
    float InfluenceRadius = 0.0f;
    Vector3 Position{0.0f, 0.0f, 0.0f};
    Vector3 BoxCenter{0.0f, 0.0f, 0.0f};
    Vector3 BoxHalfExtents{0.0f, 0.0f, 0.0f};
    Vector3 BoxAxisX{1.0f, 0.0f, 0.0f};
    Vector3 BoxAxisY{0.0f, 1.0f, 0.0f};
    Vector3 BoxAxisZ{0.0f, 0.0f, 1.0f};
    bool HasBox = false;
    uint64_t SourceKey = 0;
};

struct ProbeVolume
{
    Vector3 Center{0.0f, 0.0f, 0.0f};
    Vector3 AxisX{1.0f, 0.0f, 0.0f};
    Vector3 AxisY{0.0f, 1.0f, 0.0f};
    Vector3 AxisZ{0.0f, 0.0f, 1.0f};
    Vector3 HalfExtents{0.0f, 0.0f, 0.0f};
    bool Valid = false;
};

// The probe the camera missed by the least, recorded while the selection loop runs so
// the no-probe-selected warning can name the closest fix. A rejection on its own is a
// routine outcome in a multi-probe scene (each probe covers its own room); only the
// selection OUTCOME — no probe accepted the camera — is a diagnostic.
struct NearestRejectedProbe
{
    bool Valid = false;
    uint32 EntityId = 0;
    // How far the camera overshoots the volume, in world units: worst-axis overshoot
    // for a box (exact when one axis rejects, a lower bound when several do), radial
    // overshoot past the surface for an ellipsoid.
    float OvershootWorld = 0.0f;
    int WorstAxis = -1; // 0/1/2 = box local axis; -1 = ellipsoid (radial) rejection
    Vector3 CameraLocal{0.0f, 0.0f, 0.0f};
    Vector3 HalfExtents{0.0f, 0.0f, 0.0f};
};

void RecordIfNearest(NearestRejectedProbe& nearest, uint32 entityId, float overshootWorld,
                     int worstAxis, const Vector3& cameraLocal, const Vector3& halfExtents)
{
    if (nearest.Valid && overshootWorld >= nearest.OvershootWorld)
        return;
    nearest.Valid = true;
    nearest.EntityId = entityId;
    nearest.OvershootWorld = overshootWorld;
    nearest.WorstAxis = worstAxis;
    nearest.CameraLocal = cameraLocal;
    nearest.HalfExtents = halfExtents;
}

void ClearProbeSourceIfActive(ImageBasedLightingFeature* feature, uint64_t worldId)
{
    if (!feature)
        return;
    if (auto* probeSource =
            dynamic_cast<SceneReflectionProbeEnvironmentSource*>(feature->GetEnvironmentSource()))
    {
        // The IBL feature is engine-global but this system ticks per world: a
        // probe-less world (preview, play-mode isolation) must not tear down a
        // source another world's probe installed — that destroy/recreate churn
        // reallocated the six capture views every frame and no capture ever
        // completed. Only the owning world clears it.
        if (probeSource->GetProbe().WorldId == worldId)
            feature->ClearEnvironmentSource();
    }
    else if (auto* skySource = dynamic_cast<SkyEnvironmentSource*>(feature->GetEnvironmentSource()))
        skySource->ClearProbeOverrides();
}

bool IsBetterCandidate(const ProbeCandidate& candidate, const ProbeCandidate& best)
{
    if (candidate.Priority != best.Priority)
        return candidate.Priority > best.Priority;
    return candidate.NormalizedDistanceSq < best.NormalizedDistanceSq;
}

ProbeVolume ExtractProbeVolume(const Components::WorldTransform& xf)
{
    ProbeVolume volume{};
    const float* m = xf.matrix;
    volume.Center = xf.GetPosition();

    const Vector3 ax(m[0], m[1], m[2]);
    const Vector3 ay(m[4], m[5], m[6]);
    const Vector3 az(m[8], m[9], m[10]);
    const float sx = std::sqrt(ax.LengthSquared());
    const float sy = std::sqrt(ay.LengthSquared());
    const float sz = std::sqrt(az.LengthSquared());
    if (sx <= kMinProbeExtent || sy <= kMinProbeExtent || sz <= kMinProbeExtent)
        return volume;

    const float invSx = 1.0f / sx;
    const float invSy = 1.0f / sy;
    const float invSz = 1.0f / sz;
    volume.AxisX = ax * invSx;
    volume.AxisY = ay * invSy;
    volume.AxisZ = az * invSz;
    volume.HalfExtents = Vector3(sx * 0.5f, sy * 0.5f, sz * 0.5f);
    volume.Valid = true;
    return volume;
}

Vector3 ProjectToVolumeLocal(const ProbeVolume& volume, const Vector3& worldPos)
{
    const Vector3 d = worldPos - volume.Center;
    return Vector3(Vector3::Dot(d, volume.AxisX), Vector3::Dot(d, volume.AxisY), Vector3::Dot(d, volume.AxisZ));
}

float ComputeBoxCaptureRadius(const ProbeVolume& volume, const Vector3& capturePosition)
{
    const Vector3 captureLocal = ProjectToVolumeLocal(volume, capturePosition);
    const Vector3 reach(volume.HalfExtents.x + std::fabs(captureLocal.x), volume.HalfExtents.y + std::fabs(captureLocal.y),
                        volume.HalfExtents.z + std::fabs(captureLocal.z));
    return std::sqrt(reach.LengthSquared());
}

float ComputeEllipsoidDistanceToEdge(const Vector3& local, const Vector3& half, float normalizedRadius)
{
    if (normalizedRadius <= kMinProbeExtent)
        return std::min({half.x, half.y, half.z});

    const float localLen = std::sqrt(local.LengthSquared());
    if (localLen <= kMinProbeExtent)
        return std::min({half.x, half.y, half.z});

    return std::max(0.0f, localLen * ((1.0f / normalizedRadius) - 1.0f));
}

void CopyVolumeToCandidate(const ProbeVolume& volume, ProbeCandidate& candidate)
{
    candidate.BoxCenter = volume.Center;
    candidate.BoxHalfExtents = volume.HalfExtents;
    candidate.BoxAxisX = volume.AxisX;
    candidate.BoxAxisY = volume.AxisY;
    candidate.BoxAxisZ = volume.AxisZ;
}
} // namespace

void ReflectionProbeSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    RenderServices* rs = m_RenderServices;
    if (!rs)
        return;

    Vector3 cameraPos{0.0f, 0.0f, 0.0f};
    const std::optional<Camera> activeCamera = FindActiveCamera(world);
    const bool hasCamera = activeCamera.has_value();
    if (hasCamera)
    {
        const float* m = activeCamera->worldTransform.Data();
        cameraPos = Vector3(m[12], m[13], m[14]);
    }

    ProbeCandidate best{};
    bool found = false;
    NearestRejectedProbe nearestRejected{};

    auto q = world.Query<
        ECS::Read<Components::ReflectionProbe>,
        ECS::Read<Components::WorldTransform>>();

    q
     .Each([&](ECS::EntityHandle e,
               const Components::ReflectionProbe& probe,
               const Components::WorldTransform& xf) {
        const ProbeVolume volume = ExtractProbeVolume(xf);
        if (!volume.Valid)
            return;

        float normalizedDistanceSq = 0.0f;
        float weight = 1.0f;
        const Vector3 capturePosition = volume.Center + volume.AxisX * probe.OriginOffsetX +
                                        volume.AxisY * probe.OriginOffsetY +
                                        volume.AxisZ * probe.OriginOffsetZ;
        const bool hasBox = probe.BoxProjection;
        const Vector3 originOffsetLocal(probe.OriginOffsetX, probe.OriginOffsetY, probe.OriginOffsetZ);
        const float influenceRadius = hasBox
                                          ? ComputeBoxCaptureRadius(volume, capturePosition)
                                          : std::max({volume.HalfExtents.x, volume.HalfExtents.y, volume.HalfExtents.z}) +
                                                std::sqrt(originOffsetLocal.LengthSquared());
        if (hasCamera && hasBox)
        {
            const Vector3 local = ProjectToVolumeLocal(volume, cameraPos);
            const float ax = std::fabs(local.x);
            const float ay = std::fabs(local.y);
            const float az = std::fabs(local.z);
            if (ax > volume.HalfExtents.x ||
                ay > volume.HalfExtents.y ||
                az > volume.HalfExtents.z)
            {
                const Vector3 overshoot(ax - volume.HalfExtents.x,
                                        ay - volume.HalfExtents.y,
                                        az - volume.HalfExtents.z);
                const int worst = overshoot.x > overshoot.y
                                      ? (overshoot.x > overshoot.z ? 0 : 2)
                                      : (overshoot.y > overshoot.z ? 1 : 2);
                RecordIfNearest(nearestRejected, e.id, overshoot[worst], worst, local,
                                volume.HalfExtents);
                return;
            }

            const float nx = local.x / volume.HalfExtents.x;
            const float ny = local.y / volume.HalfExtents.y;
            const float nz = local.z / volume.HalfExtents.z;
            normalizedDistanceSq = (nx * nx + ny * ny + nz * nz) / 3.0f;

            const float distanceToEdge = std::min({
                volume.HalfExtents.x - ax,
                volume.HalfExtents.y - ay,
                volume.HalfExtents.z - az});
            const float blend = std::clamp(probe.BlendDistance, 0.0f, std::min({
                volume.HalfExtents.x,
                volume.HalfExtents.y,
                volume.HalfExtents.z}));
            if (blend > 0.0001f && distanceToEdge < blend)
            {
                weight = Math::Clamp01(distanceToEdge / blend);
                if (weight <= 0.0f)
                    return;
            }
        }
        else if (hasCamera)
        {
            const Vector3 local = ProjectToVolumeLocal(volume, cameraPos);
            const float nx = local.x / volume.HalfExtents.x;
            const float ny = local.y / volume.HalfExtents.y;
            const float nz = local.z / volume.HalfExtents.z;
            const float normalizedRadiusSq = nx * nx + ny * ny + nz * nz;
            if (normalizedRadiusSq > 1.0f)
            {
                // Radial overshoot past the ellipsoid surface, in world units: the camera
                // sits at localLen, the surface at localLen / normalizedRadius along the
                // same ray (normalizedRadius > 1 here).
                const float localLen = std::sqrt(local.LengthSquared());
                const float normalizedRadius = std::sqrt(normalizedRadiusSq);
                RecordIfNearest(nearestRejected, e.id,
                                localLen * (1.0f - 1.0f / normalizedRadius), -1, local,
                                volume.HalfExtents);
                return;
            }
            const float normalizedRadius = std::sqrt(std::max(0.0f, normalizedRadiusSq));
            normalizedDistanceSq = normalizedRadiusSq;

            const float blend = std::clamp(probe.BlendDistance, 0.0f, influenceRadius);
            if (blend > 0.0001f)
            {
                const float distanceToEdge = ComputeEllipsoidDistanceToEdge(
                    local, volume.HalfExtents, normalizedRadius);
                if (distanceToEdge < blend)
                    weight = Math::Clamp01(distanceToEdge / blend);
                if (weight <= 0.0f)
                    return;
            }
        }

        ProbeCandidate candidate{};
        candidate.Probe = probe;
        candidate.Priority = probe.Priority;
        candidate.NormalizedDistanceSq = normalizedDistanceSq;
        candidate.Weight = weight;
        candidate.InfluenceRadius = influenceRadius;
        candidate.Position = capturePosition;
        candidate.HasBox = hasBox;
        if (hasBox)
            CopyVolumeToCandidate(volume, candidate);
        candidate.SourceKey = (static_cast<uint64_t>(world.GetWorldId()) << 32u) ^
                              static_cast<uint64_t>(e.id);

        if (!found || IsBetterCandidate(candidate, best))
        {
            best = candidate;
            found = true;
        }
    });

    if (!found)
    {
        // Selection is camera-driven, so when every enabled probe rejects the camera the
        // IBL silently reverts to the sky and nothing on screen says so — every reflective
        // surface simply loses the probe content. Warn once per lost-reflections episode
        // (latched until a probe is accepted again), naming the probe the camera missed by
        // the least. A world with no probes at all records no rejection and stays silent.
        if (nearestRejected.Valid && !m_LoggedNoProbeSelected)
        {
            if (nearestRejected.WorstAxis >= 0)
                Logger::Log::Warning(
                    "No ReflectionProbe contributes: the active camera is outside every "
                    "enabled probe's influence volume, so reflections fall back to the sky. "
                    "Nearest is the box probe on entity {}, missed by {:.2f} on its local {} "
                    "axis (camera local ({:.2f}, {:.2f}, {:.2f}) against half-extents "
                    "({:.2f}, {:.2f}, {:.2f})). Grow that probe's Transform.scale on that "
                    "axis, or move the camera inside.",
                    nearestRejected.EntityId, nearestRejected.OvershootWorld,
                    "XYZ"[nearestRejected.WorstAxis], nearestRejected.CameraLocal.x,
                    nearestRejected.CameraLocal.y, nearestRejected.CameraLocal.z,
                    nearestRejected.HalfExtents.x, nearestRejected.HalfExtents.y,
                    nearestRejected.HalfExtents.z);
            else
                Logger::Log::Warning(
                    "No ReflectionProbe contributes: the active camera is outside every "
                    "enabled probe's influence volume, so reflections fall back to the sky. "
                    "Nearest is the ellipsoid probe on entity {}, missed by {:.2f} radially "
                    "(camera local ({:.2f}, {:.2f}, {:.2f}) against half-extents ({:.2f}, "
                    "{:.2f}, {:.2f})). Grow that probe's Transform.scale, or move the camera "
                    "inside.",
                    nearestRejected.EntityId, nearestRejected.OvershootWorld,
                    nearestRejected.CameraLocal.x, nearestRejected.CameraLocal.y,
                    nearestRejected.CameraLocal.z, nearestRejected.HalfExtents.x,
                    nearestRejected.HalfExtents.y, nearestRejected.HalfExtents.z);
            m_LoggedNoProbeSelected = true;
        }
        ClearProbeSourceIfActive(rs->GetFeature<ImageBasedLightingFeature>(), world.GetWorldId());
        return;
    }
    m_LoggedNoProbeSelected = false; // a probe contributes again — rearm the warning

    auto* device = rs->GetDevice();
    if (!device)
        return;

    auto& feature = rs->EnsureFeature<ImageBasedLightingFeature>();
    if (!feature.IsInitialized())
        feature.Initialize(device);
    if (!feature.IsInitialized())
        return;

    SceneReflectionProbeEnvironmentDesc desc{};
    desc.Position = best.Position;
    desc.InfluenceRadius = std::max(0.0f, best.InfluenceRadius);
    desc.BoxProjection = best.HasBox;
    if (best.HasBox)
    {
        desc.BoxCenter = best.BoxCenter;
        desc.BoxHalfExtents = best.BoxHalfExtents;
        desc.BoxAxisX = best.BoxAxisX;
        desc.BoxAxisY = best.BoxAxisY;
        desc.BoxAxisZ = best.BoxAxisZ;
    }
    desc.MaxDistance = std::max(0.0f, best.Probe.MaxDistance);
    desc.CapturedLayers = best.Probe.CapturedLayers;
    desc.WorldId = world.GetWorldId();
    desc.SourceKey = best.SourceKey != 0 ? best.SourceKey : 1u;
    desc.Intensity = std::max(0.0f, best.Probe.Intensity) * best.Weight;
    desc.ExposureEV = best.Probe.ExposureEV;
    desc.RotationRadians = best.Probe.RotationDegrees * kReflectionProbeDegToRad;
    desc.LowerHemisphereDarkness = Math::Clamp01(best.Probe.IblLowerHemisphereDarkness);
    desc.CaptureEnvironment = best.Probe.CaptureEnvironment;
    desc.CaptureResolution =
        ImageBasedLightingFeature::NormalizeCaptureResolution(best.Probe.CaptureResolution);
    desc.UpdateMode = ToSceneUpdateMode(best.Probe.UpdateMode);
    desc.RealtimeUpdateInterval = std::max(0.0f, best.Probe.RealtimeUpdateInterval);

    if (auto* sceneSource = dynamic_cast<SceneReflectionProbeEnvironmentSource*>(feature.GetEnvironmentSource()))
    {
        sceneSource->SetProbe(desc);
    }
    else
    {
        auto source = std::make_unique<SceneReflectionProbeEnvironmentSource>(*rs);
        source->SetProbe(desc);
        feature.SetEnvironmentSource(std::move(source));
    }
}

} } // namespace GameEngine::Engine::Renderer
