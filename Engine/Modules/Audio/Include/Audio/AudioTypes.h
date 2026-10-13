#pragma once

#include <cstdint>
#include <limits>

namespace GameEngine::Audio
{
using AudioWorldId = std::uint16_t;
using AudioBusId = std::uint16_t;

enum class AudioLoadPolicy : std::uint8_t
{
    Auto = 0,        // engine decides (default)
    DecodeToPCM,     // fully decode at load
    Stream,          // never fully decode; stream/decode on demand
};

struct AudioRuntimeSettings
{
    AudioLoadPolicy loadPolicy = AudioLoadPolicy::Auto;
    bool allowVirtualization = true;
};

struct AudioSystemConfig
{
    std::uint32_t maxListeners = 4;
    std::uint32_t maxEmitters = 4096;
    std::uint32_t maxVoices = 256;
    std::uint32_t mixSampleRate = 48000;
    std::uint32_t mixBufferSizeFrames = 512;
    bool enable3D = true;
};

struct ListenerState
{
    float position[3] = {0, 0, 0};
    float forward[3] = {0, 0, 1}; // local +Z; matches WorldTransform column 2
    float up[3] = {0, 1, 0};
    float velocity[3] = {0, 0, 0};
};

// Distance attenuation curve of a spatialized voice; each maps to the miniaudio
// model of the same name.
enum class AudioAttenuationModel : std::uint8_t
{
    None = 0,    // no distance attenuation
    Inverse,     // 1/d-shaped falloff (default)
    Linear,      // straight ramp between minDistance and maxDistance
    Exponential, // (d / minDistance)^-rolloff
};

/** Distance falloff of a spatialized voice; 2D voices ignore it. Distances are in
 *  world units from the listener. With dc = clamp(d, minDistance, maxDistance):
 *    Inverse:     gain = minDistance / (minDistance + rolloff * (dc - minDistance))
 *    Linear:      gain = 1 - rolloff * (dc - minDistance) / (maxDistance - minDistance), floored at 0
 *    Exponential: gain = (dc / minDistance)^-rolloff
 *    None:        gain = 1
 *  Inside minDistance a voice plays at full volume; past maxDistance its gain holds at
 *  the maxDistance value (Linear with rolloff 1 is silent there, Inverse is not).
 *  maxDistance equal to minDistance disables attenuation. The defaults are miniaudio's
 *  (Inverse, 1, no limit, 1: gain = 1/d beyond one unit).
 *  A voice sanitizes the values when it starts: a NaN or infinite value takes its
 *  default (an infinite maxDistance is the same as no limit), minDistance is raised to
 *  at least the smallest positive normal float (Inverse and Exponential divide by it),
 *  maxDistance to at least minDistance, and rolloff to at least 0. */
struct AudioFalloff
{
    AudioAttenuationModel model = AudioAttenuationModel::Inverse;
    float minDistance = 1.0f;
    float maxDistance = std::numeric_limits<float>::max(); // no limit
    float rolloff = 1.0f;
};

struct PlayOptions
{
    float volume = 1.0f;
    float pitch = 1.0f;
    bool loop = false;
    bool spatialized = false;
    AudioBusId bus = 0;
    AudioFalloff falloff{}; // spatialized voices only
};

} // namespace GameEngine::Audio



