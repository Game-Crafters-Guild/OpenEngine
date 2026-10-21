#include "ECSModules/Audio/Systems/AudioEmitterSystem.h"

#include "Audio/AudioSystem.h"
#include "AssetCore/GUID.h"
#include "Components/Audio/AudioEmitter.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/SwapGenerationGuard.h"
#include "ECS/World.h"

#include <cmath>
#include <unordered_map>

namespace GameEngine { namespace Engine::Audio {
using namespace ::GameEngine::Audio;

using GameEngine::Components::AudioEmitter;
using GameEngine::Components::AudioEmitterAttenuation;
using GameEngine::Components::WorldTransform;

namespace
{
struct RuntimeEmitter
{
    GUID guid{};
    Audio::AudioClipHandle clip = Audio::INVALID_AUDIO_CLIP_HANDLE;
    Audio::AudioEmitterHandle voice = Audio::INVALID_AUDIO_EMITTER_HANDLE;
    bool started = false; // played once for playOnStart semantics
    uint16_t busUsedAtStart = 2; // bus we used when starting the voice; if emitter.bus changes, we restart to re-route
    Audio::AudioFalloff falloffApplied{}; // the component's falloff as last handed to the voice
    // The component's volume and pitch as last handed to the voice.
    float32 volumeHanded = 1.0f;
    float32 pitchHanded = 1.0f;
};

// Equal, with NaN equal to NaN: a component value the voice refuses (a non-finite one, with
// its warning) is handed once, not on every frame.
bool SameValue(float32 a, float32 b)
{
    return a == b || (std::isnan(a) && std::isnan(b));
}

// Stops the voice and re-arms playOnStart, so the emitter plays again from the
// start once it is back on.
void StopEmitterVoice(Audio::AudioSystem& audio, RuntimeEmitter& rt)
{
    if (rt.voice.IsValid())
    {
        audio.Stop(rt.voice);
        rt.voice = Audio::INVALID_AUDIO_EMITTER_HANDLE;
    }
    rt.started = false;
}

Audio::AudioAttenuationModel ToAttenuationModel(AudioEmitterAttenuation attenuation)
{
    switch (attenuation)
    {
    case AudioEmitterAttenuation::None:
        return Audio::AudioAttenuationModel::None;
    case AudioEmitterAttenuation::Linear:
        return Audio::AudioAttenuationModel::Linear;
    case AudioEmitterAttenuation::Exponential:
        return Audio::AudioAttenuationModel::Exponential;
    case AudioEmitterAttenuation::Inverse:
        break;
    }
    return Audio::AudioAttenuationModel::Inverse;
}

Audio::AudioFalloff MakeFalloff(const AudioEmitter& emitter)
{
    Audio::AudioFalloff falloff{};
    falloff.model = ToAttenuationModel(emitter.attenuation);
    falloff.minDistance = emitter.minDistance;
    falloff.maxDistance = emitter.maxDistance;
    falloff.rolloff = emitter.rolloff;
    return falloff;
}

} // namespace

struct AudioEmitterSystem::Impl
{
    std::unordered_map<ECS::EntityHandle, RuntimeEmitter, ECS::EntityHandleHash> emitters;

    // Last-consumed WorldReset generation (lifecycle events, §6 Q4): Clear()
    // fires no per-entity Removed events, so scene unload is detected here.
    // Deliberately NOT absorbed into the swap guard below: reset handling is
    // domain-specific (it stops voices for a scene that is gone).
    uint64 lastResetGeneration = 0;

    // Cadence guard over the lifecycle swap generation: the editor's
    // play-mode pause disables this system via SetRenderingSystemEnabled
    // while the engine tick keeps swapping, so events recorded during the
    // pause are promoted and discarded unseen. A missed window on resume
    // triggers a one-shot stale sweep of the voice map — the poll this
    // system's event conversion replaced, run exactly once per gap.
    ECS::SwapGenerationGuard swapGuard;
};

AudioEmitterSystem::AudioEmitterSystem(Audio::AudioSystem* audioSystem)
    : m_Impl(std::make_unique<Impl>()),
      m_Audio(audioSystem)
{
}

AudioEmitterSystem::~AudioEmitterSystem() = default;

void AudioEmitterSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    // Cadence note (lifecycle events have no poll fallback): any window this
    // system does not consume — audio still async-initializing at startup,
    // or the system disabled by the editor's play-mode pause gating
    // (SetRenderingSystemEnabled) while the engine tick keeps swapping — is
    // healed by the swap-generation gap sweep below. Missed Added events are
    // additionally absorbed by the query's lazy insert.
    if (!m_Audio || !m_Audio->IsInitialized())
        return;
    if (!m_Impl)
        return;

    // World reset (scene unload / World::Clear): no per-entity Removed
    // events fire (§6 Q4) — drop ALL per-entity bookkeeping, stopping any
    // still-live voices first. Runs before the query so records created for
    // the new scene are untouched.
    const uint64 resetGeneration = world.GetLifecycleResetGeneration();
    if (resetGeneration != m_Impl->lastResetGeneration)
    {
        m_Impl->lastResetGeneration = resetGeneration;
        for (auto& [handle, rt] : m_Impl->emitters)
        {
            if (rt.voice.IsValid())
                m_Audio->Stop(rt.voice);
        }
        m_Impl->emitters.clear();
    }

    // Cadence-gap heal (ECS/SwapGenerationGuard.h): a missed window means at
    // least one whole event window was swapped out while this system wasn't
    // running (play-mode pause disables it; the swap keeps ticking) — any
    // Removed<AudioEmitter> or GetDisabled<AudioEmitter> in that window is
    // gone, and a looping voice for a deleted or switched-off emitter would
    // otherwise play on until play-exit. Run the stale sweep the event
    // conversion replaced, exactly once per gap: stop + drop every record
    // whose entity is dead or no longer carries the component, and stop the
    // voice of every emitter that is off.
    // The guard read stays below the IsInitialized early-return above by
    // design: the generations accumulated while audio async-initializes make
    // the first consumed frame sweep once, covering entities that died
    // before this system ever ran.
    if (m_Impl->swapGuard.ConsumeAndCheckMissed(world.GetWorldId(),
                                                world.GetLifecycleSwapGeneration()))
    {
        for (auto it = m_Impl->emitters.begin(); it != m_Impl->emitters.end();)
        {
            if (!world.IsValid(it->first) || !world.HasComponent<AudioEmitter>(it->first))
            {
                if (it->second.voice.IsValid())
                    m_Audio->Stop(it->second.voice);
                it = m_Impl->emitters.erase(it);
                continue;
            }
            const ECS::Entity entity(&world, it->first);
            if (!entity.IsEnabledInHierarchy() || !entity.IsEnabled<AudioEmitter>())
                StopEmitterVoice(*m_Audio, it->second);
            ++it;
        }
    }

    // Setup lane: seed runtime records for newly-added emitters
    // (Added<AudioEmitter>, the §6.2 init pattern). The query below still
    // lazy-inserts — an entity can be query-visible one frame before its
    // Added event surfaces (the documented visibility inversion) — so this
    // is a warm-up, not a correctness dependency; the event subscription
    // exists primarily for the teardown lane below.
    for (ECS::EntityHandle e : world.GetAdded<AudioEmitter>())
    {
        m_Impl->emitters.try_emplace(e);
    }

    // Teardown lane for an emitter switched off (its own tag or its entity):
    // the query below no longer visits it, so its voice is stopped here.
    for (ECS::EntityHandle e : world.GetDisabled<AudioEmitter>())
    {
        if (auto it = m_Impl->emitters.find(e); it != m_Impl->emitters.end())
            StopEmitterVoice(*m_Audio, it->second);
    }

    auto q = world.Query<ECS::Read<AudioEmitter>, ECS::Optional<WorldTransform>,
                         ECS::Optional<Components::Transform>>();
    q.Each([&](ECS::EntityHandle e, const AudioEmitter& emitter, const WorldTransform* wt,
               const Components::Transform* local)
           {
               // A pose the hierarchy has not resolved yet (Version 0 beside a
               // Transform) would start the voice at the origin; the emitter
               // starts on the frame its pose resolves.
               if (wt && local && wt->Version == 0u)
                   return;
               auto& rt = m_Impl->emitters[e];

               const GUID guid = emitter.clipGuid.ToGuid(); // null GUID when unset
               const bool guidChanged = (guid != rt.guid);
               if (guidChanged)
               {
                   // Stop any previous voice.
                   if (rt.voice.IsValid())
                   {
                       m_Audio->Stop(rt.voice);
                       rt.voice = Audio::INVALID_AUDIO_EMITTER_HANDLE;
                   }
                   rt.guid = guid;
                   rt.started = false;
                   rt.clip = guid.IsNull() ? Audio::INVALID_AUDIO_CLIP_HANDLE : m_Audio->ResolveClip(guid);
               }

               // No clip => stop voice and bail.
               if (guid.IsNull())
               {
                   if (rt.voice.IsValid())
                   {
                       m_Audio->Stop(rt.voice);
                       rt.voice = Audio::INVALID_AUDIO_EMITTER_HANDLE;
                   }
                   return;
               }

               // If voice handle expired (one-shots), clear but do not restart unless not started yet.
               if (rt.voice.IsValid() && !m_Audio->IsAlive(rt.voice))
               {
                   rt.voice = Audio::INVALID_AUDIO_EMITTER_HANDLE;
               }

               // If user changed bus/channel in the inspector, stop current voice so we restart on the new bus.
               if (rt.voice.IsValid() && emitter.bus != rt.busUsedAtStart)
               {
                   m_Audio->Stop(rt.voice);
                   rt.voice = Audio::INVALID_AUDIO_EMITTER_HANDLE;
                   rt.started = false;
               }

               const bool spatialize = emitter.spatialized && (wt != nullptr);

               Audio::PlayOptions opts{};
               opts.volume = emitter.volume;
               opts.pitch = emitter.pitch;
               opts.loop = emitter.loop;
               opts.spatialized = spatialize;
               opts.bus = static_cast<Audio::AudioBusId>(emitter.bus);
               opts.falloff = MakeFalloff(emitter);

               // Start if requested.
               const bool shouldStart = !rt.voice.IsValid() && (emitter.loop || (emitter.playOnStart && !rt.started));
               if (shouldStart)
               {
                   if (spatialize)
                   {
                       const float32* m = wt->matrix;
                       Mathematics::Vector3 pos{m[12], m[13], m[14]};
                       Mathematics::Vector3 v{0, 0, 0};

                       rt.voice = m_Audio->Play3D(rt.clip, static_cast<Audio::AudioWorldId>(emitter.worldId), pos, v, opts);
                   }
                   else
                   {
                       rt.voice = m_Audio->Play2D(rt.clip, opts);
                   }

                   if (rt.voice.IsValid())
                   {
                       rt.started = true;
                       rt.busUsedAtStart = emitter.bus;
                       rt.falloffApplied = opts.falloff;
                       rt.volumeHanded = emitter.volume;
                       rt.pitchHanded = emitter.pitch;
                   }
               }

               // Volume, pitch and a spatialized voice's distance falloff follow the
               // component for the life of the voice: a changed value is handed to it
               // once, whatever the voice makes of it.
               if (rt.voice.IsValid())
               {
                   if (!SameValue(emitter.volume, rt.volumeHanded))
                   {
                       m_Audio->SetVolume(rt.voice, emitter.volume);
                       rt.volumeHanded = emitter.volume;
                   }
                   if (!SameValue(emitter.pitch, rt.pitchHanded))
                   {
                       m_Audio->SetPitch(rt.voice, emitter.pitch);
                       rt.pitchHanded = emitter.pitch;
                   }
                   if (spatialize && opts.falloff != rt.falloffApplied)
                   {
                       m_Audio->SetFalloff(rt.voice, opts.falloff);
                       rt.falloffApplied = opts.falloff;
                   }
               }

               // Keep 3D state updated.
               if (rt.voice.IsValid() && spatialize)
               {
                   const float32* m = wt->matrix;
                   Mathematics::Vector3 pos{m[12], m[13], m[14]};
                   Mathematics::Vector3 v{0, 0, 0};

                   m_Audio->SetEmitter3D(rt.voice, static_cast<Audio::AudioWorldId>(emitter.worldId), pos, v);
               }
           });

    // Teardown lane: event-driven (Removed<AudioEmitter>), replacing the
    // former per-frame `seen` set + full-map stale scan. Covers component
    // removal and every per-entity destroy path (immediate, deferred,
    // editor undo's preserve-handle destroy) one frame after the fact; the
    // handle is an ID only — the entity may already be dead — which is all
    // the side map needs. Scene unload is the reset-generation branch above.
    for (ECS::EntityHandle e : world.GetRemoved<AudioEmitter>())
    {
        auto it = m_Impl->emitters.find(e);
        if (it == m_Impl->emitters.end())
            continue;

        // Removed-then-re-added within one event window (component swap,
        // undo revive+restore): the record is still live — the query above
        // already reconciled clip/voice state via its guidChanged path.
        if (world.HasComponent<AudioEmitter>(e))
            continue;

        if (it->second.voice.IsValid())
        {
            m_Audio->Stop(it->second.voice);
        }
        m_Impl->emitters.erase(it);
    }
}

} } // namespace GameEngine::Engine::Audio


