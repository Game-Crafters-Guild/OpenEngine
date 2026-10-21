#pragma once

#include "Components/AssetRef.h"
#include "Types/Types.h"

#include <type_traits>

namespace GameEngine::Components
{

// Distance attenuation curve of a spatialized emitter (Audio::AudioAttenuationModel).
enum class AudioEmitterAttenuation : uint8
{
    None = 0,
    Inverse,
    Linear,
    Exponential,
};

// Marks an entity as an audio emitter.
// The Audio ECS systems will create/update a playing voice in the engine-owned AudioSystem.
struct AudioEmitter
{
    AudioRef clipGuid{};

    bool spatialized = true;  // true => 3D, false => 2D
    bool loop = false;
    bool playOnStart = true;  // play once when first seen (or when clipGuid changes)

    uint16 worldId = 0;       // reserved for future multi-world routing
    uint16 bus = 2;           // 0=Master, 1=Music, 2=SFX, 3=UI, 4=VO, 5=Aux

    float32 volume = 1.0f;
    float32 pitch = 1.0f;

    // Distance falloff of a spatialized emitter, in world units from the listener; the
    // gain curves and the sanitizing rules are Audio::AudioFalloff's. An edit reaches
    // the playing voice, as volume and pitch do. The defaults give gain = 1/distance
    // from one unit out to 10000 units.
    AudioEmitterAttenuation attenuation = AudioEmitterAttenuation::Inverse;
    float32 minDistance = 1.0f;                                   // full volume inside this distance
    float32 maxDistance = 10000.0f;                               // the gain stops changing past here
    float32 rolloff = 1.0f;                                       // steepness of the curve
};

static_assert(std::is_trivially_copyable_v<AudioEmitter>, "AudioEmitter must be trivially copyable");
static_assert(std::is_standard_layout_v<AudioEmitter>, "AudioEmitter must be standard layout");

} // namespace GameEngine::Components
