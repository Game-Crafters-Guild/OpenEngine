#pragma once

// A script API with every shape the binding generators emit, once. ComponentScannerTests compares
// the generated native and C# bindings of it with the files under Expected/; ScriptBindingTests
// compiles the native binding against the bodies in ProbeScriptApi.cpp and calls it.

#include "ECS/ECS.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/Quaternion.h"
#include "Mathematics/Vector3.h"
#include "Rendering/RenderLayerMask.h"
#include "Types/Color.h"
#include "Types/ScriptCallback.h"
#include "Types/StringId.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace GameEngine::Probe
{

class ProbeSystem;

/// How loud a sound plays.
// @ge-script
enum class Loudness : std::int16_t
{
    Quiet = -1,
    Normal,
    Loud = 4,
};

inline constexpr std::uint32_t kAllLayers = 0xFFFFFFFFu;

/// The speakers a sound reaches; they combine with |.
// @ge-script @ge-script-flags
enum class Speakers : std::uint8_t
{
    None = 0,
    Left = 1,
    Right = 2,
};

/// The options of one sound.
///
/// Every field has its default.
// @ge-script
struct SoundOptions
{
    bool loop = false;
    float volume = 1.0f;
    Loudness loudness = Loudness::Normal;
    std::uint32_t layers = kAllLayers;
    std::int64_t seed = -3;
};

/// Where a ray met a surface.
// @ge-script
struct Hit
{
    Mathematics::Vector3 point;
    float distance = 0.0f;
    Mathematics::Quaternion facing;
};

/// Two entities touched.
// @ge-script
struct Touch
{
    ECS::EntityHandle self;
    ECS::EntityHandle other;
    bool entering = true;
    float force = 0.0f;
};

} // namespace GameEngine::Probe

namespace GameEngine::Probe::Script
{

/// Counts the calls that reached the probe; on any thread.
// @ge-script @ge-any-thread
std::uint32_t CallCount();

/// Plays a sound by id, and answers whether it plays.
// @ge-script @ge-context
bool Play(ProbeSystem& probe, StringId sound, const SoundOptions& options = {}, float gain = 0.5f,
          Loudness loudness = Loudness::Loud, std::uint32_t layers = kAllLayers);

/// Finds the nearest hit along a ray.
// @ge-script @ge-script-try @ge-context @ge-engine-thread
bool TryCast(ProbeSystem& probe, Mathematics::Vector3 origin, const Mathematics::Vector3& direction, Hit& outHit,
             float maxDistance = 1000.0f);

/// The pose of the probe, as a matrix.
// @ge-script @ge-context
Mathematics::Matrix4x4 GetPose(ProbeSystem& probe);

/// Sets the flag `on`.
// @ge-script @ge-context
void SetFlag(ProbeSystem& probe, bool on, std::int8_t level, double weight);

/// The options the last Play used.
// @ge-script @ge-context
SoundOptions GetLastOptions(ProbeSystem& probe);

/// Throws, so the binding's catch answers.
// @ge-script
void Throw();

/// Pushes the entity.
// @ge-script @ge-context
void Push(ProbeSystem& probe, ECS::EntityHandle entity, Mathematics::Vector3 force);

/// The speed of the entity.
// @ge-script @ge-context
float GetSpeed(ProbeSystem& probe, ECS::EntityHandle entity);

/// Sets the speed of the entity.
// @ge-script @ge-context
void SetSpeed(ProbeSystem& probe, ECS::EntityHandle entity, float speed);

/// Finds the entity nearest to a point.
// @ge-script @ge-script-try @ge-context
bool TryNearest(ProbeSystem& probe, Mathematics::Vector3 point, ECS::EntityHandle& outEntity);

/// Keeps `text` as the probe's label, and answers it.
// @ge-script @ge-context
std::string Echo(ProbeSystem& probe, std::string_view text);

/// Makes `layers` the probe's layers, and answers the layers it had.
// @ge-script @ge-context
Rendering::RenderLayerMask SwapLayers(ProbeSystem& probe, Rendering::RenderLayerMask layers);

/// Writes the probe's hits into `hits`, nearest first, and answers how many there are.
// @ge-script @ge-context
std::int32_t ListHits(ProbeSystem& probe, Mathematics::Vector3 origin, std::span<Hit> hits);

/// Makes `tint` the probe's tint, and answers the tint it had.
// @ge-script @ge-context
ColorLinear SwapTint(ProbeSystem& probe, ColorLinear tint);

/// Calls the handler when the entity touches another.
// @ge-script-event PhysicsStep
SubscriptionId SubscribeTouched(ECS::EntityHandle scope, ScriptCallback<Touch> callback);

/// Stops the calls of SubscribeTouched.
// @ge-script-event
void UnsubscribeTouched(SubscriptionId subscription);

} // namespace GameEngine::Probe::Script
