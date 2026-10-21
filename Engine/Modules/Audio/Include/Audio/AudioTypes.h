#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

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

/// Where the mixed signal goes.
enum class AudioOutput : std::uint8_t
{
    Device = 0, // the system's default playback device
    Silent,     // mixed on miniaudio's null backend: voices, metering and output
                // capture run on the same clock, and no device is opened
};

/// Parses an output name ("device" or "silent", any case, whitespace
/// ignored). Nullopt for anything else, so a caller can refuse a typo.
inline std::optional<AudioOutput> TryParseAudioOutput(std::string_view value)
{
    std::string normalized(value);
    normalized.erase(std::remove_if(normalized.begin(), normalized.end(),
                                    [](unsigned char c) { return std::isspace(c) != 0; }),
                     normalized.end());
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (normalized == "device")
        return AudioOutput::Device;
    if (normalized == "silent")
        return AudioOutput::Silent;
    return std::nullopt;
}

struct AudioSystemConfig
{
    std::uint32_t maxListeners = 4;
    std::uint32_t maxEmitters = 4096;
    std::uint32_t maxVoices = 256;
    std::uint32_t mixSampleRate = 48000;
    std::uint32_t mixBufferSizeFrames = 512;
    bool enable3D = true;
    AudioOutput output = AudioOutput::Device;
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
 *  maxDistance equal to minDistance disables attenuation. The defaults are Inverse,
 *  minDistance 1, maxDistance 10000 and rolloff 1: gain = 1/d from one unit out to
 *  10000 units, where it holds at 1/10000 (-80 dB). An infinite maxDistance is no limit.
 *  minDistance must be finite and above 0 (Inverse and Exponential divide by it, and
 *  near 0 the voice is inaudible from the first unit out): SetFalloff ignores a falloff
 *  without one, and a voice started with one takes the default, each with a warning.
 *  Otherwise a voice sanitizes the values it is given: a NaN maxDistance or a non-finite
 *  rolloff takes its default, maxDistance is raised to at least minDistance, and rolloff
 *  to at least 0. */
struct AudioFalloff
{
    AudioAttenuationModel model = AudioAttenuationModel::Inverse;
    float minDistance = 1.0f;
    float maxDistance = 10000.0f;
    float rolloff = 1.0f;

    bool operator==(const AudioFalloff&) const = default;
};

struct PlayOptions
{
    float volume = 1.0f;
    float pitch = 1.0f;
    float pan = 0.0f; // -1 (left) .. 1 (right); non-spatialized voices only
    bool loop = false;
    bool spatialized = false;
    AudioBusId bus = 0;
    AudioFalloff falloff{}; // spatialized voices only
};

} // namespace GameEngine::Audio



