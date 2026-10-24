// The bodies of the binding generators' fixture script API
// (Tools/ComponentScanner/Tests/Fixtures/Bindings), so ScriptBindingTests can call its generated
// native binding.
#include "ProbeSystem.h"

#include <algorithm>
#include <stdexcept>

namespace GameEngine::Probe::Script
{

std::uint32_t CallCount()
{
    return ProbeSystem::Calls().load();
}

bool Play(ProbeSystem& probe, StringId sound, const SoundOptions& options, float gain, Loudness loudness,
          std::uint32_t layers)
{
    ++ProbeSystem::Calls();
    probe.LastSound = sound;
    probe.LastOptions = options;
    probe.LastGain = gain;
    probe.LastLoudness = loudness;
    probe.LastLayers = layers;
    return options.volume > 0.0f;
}

bool TryCast(ProbeSystem& probe, Mathematics::Vector3 origin, const Mathematics::Vector3& direction, Hit& outHit,
             float maxDistance)
{
    ++ProbeSystem::Calls();
    if (direction.x == 0.0f && direction.y == 0.0f && direction.z == 0.0f)
        return false;
    outHit.point = origin + direction * maxDistance;
    outHit.distance = maxDistance;
    outHit.facing = probe.Facing;
    return true;
}

Mathematics::Matrix4x4 GetPose(ProbeSystem& probe)
{
    ++ProbeSystem::Calls();
    return probe.Pose;
}

void SetFlag(ProbeSystem& probe, bool on, std::int8_t level, double weight)
{
    ++ProbeSystem::Calls();
    probe.Flag = on;
    probe.Level = level;
    probe.Weight = weight;
}

SoundOptions GetLastOptions(ProbeSystem& probe)
{
    ++ProbeSystem::Calls();
    return probe.LastOptions;
}

void Throw()
{
    throw std::runtime_error("the probe throws");
}

void Push(ProbeSystem& probe, ECS::EntityHandle entity, Mathematics::Vector3 force)
{
    ++ProbeSystem::Calls();
    probe.LastEntity = entity;
    probe.LastForce = force;
}

float GetSpeed(ProbeSystem& probe, ECS::EntityHandle entity)
{
    ++ProbeSystem::Calls();
    probe.LastEntity = entity;
    return probe.Speed;
}

void SetSpeed(ProbeSystem& probe, ECS::EntityHandle entity, float speed)
{
    ++ProbeSystem::Calls();
    probe.LastEntity = entity;
    probe.Speed = speed;
}

bool TryNearest(ProbeSystem& probe, Mathematics::Vector3 point, ECS::EntityHandle& outEntity)
{
    ++ProbeSystem::Calls();
    (void)point;
    outEntity = probe.Nearest;
    return probe.Nearest.IsValid();
}

std::string Echo(ProbeSystem& probe, std::string_view text)
{
    ++ProbeSystem::Calls();
    probe.LastText = std::string(text);
    return probe.LastText;
}

Rendering::RenderLayerMask SwapLayers(ProbeSystem& probe, Rendering::RenderLayerMask layers)
{
    ++ProbeSystem::Calls();
    const Rendering::RenderLayerMask previous = probe.Layers;
    probe.Layers = layers;
    return previous;
}

std::int32_t ListHits(ProbeSystem& probe, Mathematics::Vector3 origin, std::span<Hit> hits)
{
    ++ProbeSystem::Calls();
    (void)origin;
    const std::size_t written = std::min(probe.Hits.size(), hits.size());
    std::copy_n(probe.Hits.begin(), written, hits.begin());
    return static_cast<std::int32_t>(probe.Hits.size());
}

ColorLinear SwapTint(ProbeSystem& probe, ColorLinear tint)
{
    ++ProbeSystem::Calls();
    const ColorLinear previous = probe.Tint;
    probe.Tint = tint;
    return previous;
}

SubscriptionId SubscribeTouched(ECS::EntityHandle scope, ScriptCallback<Touch> callback)
{
    ++ProbeSystem::Calls();
    ProbeSystem::TouchSubscription& touched = ProbeSystem::Touched();
    touched = {touched.Id + 1, scope, callback};
    return touched.Id;
}

void UnsubscribeTouched(SubscriptionId subscription)
{
    ProbeSystem::TouchSubscription& touched = ProbeSystem::Touched();
    if (subscription == 0 || subscription != touched.Id)
        return;
    const ScriptCallback<Touch> callback = touched.Callback;
    touched.Callback = {};
    if (callback.Ended != nullptr)
        callback.Ended(callback.User);
}

} // namespace GameEngine::Probe::Script
