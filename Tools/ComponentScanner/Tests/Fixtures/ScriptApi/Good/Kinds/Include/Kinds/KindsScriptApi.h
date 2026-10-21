#pragma once

// Every kind of the script API kind table, once. Never compiled: the scanner reads it.

namespace GameEngine::Kinds
{

class KindsSystem;

/// A sound voice.
// @ge-script-handle
using Voice = StrongHandle<VoiceTag>;

/// A pool slot.
// @ge-script-handle
using Slot = GenerationalVector::Handle;

/// How loud a voice plays.
// @ge-script
enum class Loudness : std::int16_t
{
    Quiet = -1,
    Normal,
    Loud = 4,
};

constexpr float kDefaultPitch = 1.0f;
constexpr std::uint32_t kAllLayers = 0xFFFFFFFFu;

/// Options for one voice.
///
/// Every field has its default.
// @ge-script
struct PlayOptions
{
    bool loop = false;
    float volume{1.0f};
    float pitch = kDefaultPitch;
    Loudness loudness = Loudness::Normal;
    std::uint32_t layers = kAllLayers;
    Mathematics::Vector3 offset{0.0f, 1.0f, 0.0f};
    ColorLinear tint = {1.0f, 1.0f, 1.0f, 1.0f};
    ColorLinear rim{0.25f, 0.5f, 0.75f};
    ColorLinear shade;
    ECS::EntityHandle owner;
    Voice previous{};
};

/// A hit.
// @ge-script
struct Hit
{
    std::uint8_t flags = 0;
    double distance = 0.0;
    PlayOptions options;
    Mathematics::Quaternion rotation{1.0f, 0.0f, 0.0f, 0.0f};
    Mathematics::Quaternion turn{0.0f, 0.0f, 0.0f, 1.0f};
    Mathematics::Matrix4x4 pose;
    Mathematics::Vector2 scale;
};

#if 0
// @ge-script
void Disabled(std::vector<int> ignored);
#endif

} // namespace GameEngine::Kinds

namespace GameEngine::Kinds::Script
{

/// Every scalar kind.
// @ge-script
void Scalars(bool b, std::int8_t i8, std::int16_t i16, std::int32_t i32, std::int64_t i64, std::uint8_t u8,
             std::uint16_t u16, std::uint32_t u32, std::uint64_t u64, float f, double d, int i, uint8 engineByte,
             float32 engineFloat);

/// Every math kind, and the angle.
// @ge-script
Mathematics::Matrix4x4 Math(Mathematics::Vector2 v2, const Mathematics::Vector3& v3, Mathematics::Vector4 v4,
                            Mathematics::Quaternion q, Mathematics::Radians angle, ColorLinear color);

/// Entities, ids, text and spans.
// @ge-script @ge-any-thread
StringId Ids(ECS::EntityHandle entity, StringId id, std::string_view name, std::span<const float> weights,
             std::span<const Hit> hits);

/// Plays a voice.
// @ge-script @ge-context @ge-engine-thread
Voice Play(KindsSystem& kinds, Slot slot, const PlayOptions& options = {}, float gain = 0.5f, Loudness loudness = Loudness::Loud,
           std::uint32_t layers = kAllLayers);

/// Finds the nearest hit.
// @ge-script @ge-script-try @ge-context
bool TryFindHit(KindsSystem& kinds, Mathematics::Vector3 origin, Hit& outHit, float maxDistance = 1000.0f);

/// The pitch of a voice.
float GetPitch(Voice voice); // @ge-script

/// Sets the gain, written with a suffix.
// @ge-script
void SetGain(float gain = 1.0f);

/// Sets the gain, written with a bare point.
// @ge-script
void SetTrim(float trim = 1.f, std::uint8_t mask = 0xFFu, double scale = 2.50);

} // namespace GameEngine::Kinds::Script
