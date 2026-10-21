#pragma once

#include "Audio/AudioHandles.h"
#include "Audio/AudioTypes.h"
#include "AssetCore/GUID.h"
#include "Mathematics/Vector3.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace GameEngine
{
class AssetManager;
class AudioAsset;
}

namespace GameEngine::Audio
{

class AudioSystem
{
public:
    using OutputCaptureCallback = std::function<void(const float* samples,
                                                     std::uint32_t frameCount,
                                                     std::uint32_t channels,
                                                     std::uint32_t sampleRate)>;

    explicit AudioSystem(AssetManager& assets);
    ~AudioSystem();

    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    bool Initialize(const AudioSystemConfig& config);
    void Shutdown();
    void Update(float dt);

    bool IsInitialized() const { return m_Initialized; }

    // Resolve/cache API
    AudioClipHandle ResolveClip(const GUID& guid);
    AudioClipHandle ResolveClip(const GameEngine::AudioAsset& asset);

    // Play APIs (overloads)
    AudioEmitterHandle Play2D(const GUID& guid, const PlayOptions& opts = {});
    AudioEmitterHandle Play2D(const GameEngine::AudioAsset& asset, const PlayOptions& opts = {});
    AudioEmitterHandle Play2D(AudioClipHandle clip, const PlayOptions& opts = {});

    AudioEmitterHandle Play3D(const GUID& guid,
                              AudioWorldId world,
                              const Mathematics::Vector3& pos,
                              const Mathematics::Vector3& vel,
                              const PlayOptions& opts = {});
    AudioEmitterHandle Play3D(const GameEngine::AudioAsset& asset,
                              AudioWorldId world,
                              const Mathematics::Vector3& pos,
                              const Mathematics::Vector3& vel,
                              const PlayOptions& opts = {});
    AudioEmitterHandle Play3D(AudioClipHandle clip,
                              AudioWorldId world,
                              const Mathematics::Vector3& pos,
                              const Mathematics::Vector3& vel,
                              const PlayOptions& opts = {});

    void SetListener(AudioWorldId world, std::uint32_t listenerIndex, const ListenerState& state);

    // Emitter control (used by ECS + editor)
    void SetEmitter3D(AudioEmitterHandle emitter, AudioWorldId world, const Mathematics::Vector3& pos, const Mathematics::Vector3& vel);
    void Stop(AudioEmitterHandle emitter);
    // Stop all currently alive voices (best-effort). Intended for Editor PlayMode teardown.
    void StopAllVoices();
    bool IsAlive(AudioEmitterHandle emitter) const;
    // False while the voice is pending (clip still loading) or paused.
    bool IsPlaying(AudioEmitterHandle emitter) const;

    // Live voice control. Game thread only, like Play*/Stop. Invalid or stale
    // handles are no-ops. On a pending voice the value is stored and applied
    // when the voice starts. A non-finite value (NaN, infinity) is ignored with
    // a warning naming the voice: it would make the whole mix non-finite.
    // Linear gain; negative values clamp to 0.
    void SetVolume(AudioEmitterHandle emitter, float linearVolume);
    // Playback rate multiplier (1 = original). Non-positive values are ignored.
    void SetPitch(AudioEmitterHandle emitter, float pitch);
    // Stereo pan, -1 (left) .. 1 (right), clamped. Applies to non-spatialized
    // voices only: a spatialized voice is panned by its position relative to
    // the listener, and SetPan leaves it untouched.
    void SetPan(AudioEmitterHandle emitter, float pan);
    // Current values of a live voice; 0 for an invalid or stale handle.
    float GetVolume(AudioEmitterHandle emitter) const;
    float GetPitch(AudioEmitterHandle emitter) const;
    float GetPan(AudioEmitterHandle emitter) const;

    // Pause keeps the playback position and the voice (IsAlive stays true;
    // IsPlaying reports false; a paused voice is never reclaimed as finished,
    // though it still holds its voice and is taken as the oldest when a new
    // voice needs one and none is free).
    // Resume continues from the paused position. A pending voice paused
    // before its clip is ready does not start until resumed. A one-shot that
    // had already reached its end when paused is reclaimed instead of
    // restarting.
    void Pause(AudioEmitterHandle emitter);
    void Resume(AudioEmitterHandle emitter);
    bool IsPaused(AudioEmitterHandle emitter) const;

    // Distance falloff of a spatialized voice (AudioFalloff documents the gain
    // curves and how out-of-range values are sanitized). A playing voice takes it
    // at once; a pending voice starts with it. A 2D voice has no distance, and
    // SetFalloff leaves it untouched.
    void SetFalloff(AudioEmitterHandle emitter, const AudioFalloff& falloff);
    // The falloff a spatialized voice uses, after sanitizing; the default
    // AudioFalloff for a 2D voice or an invalid or stale handle.
    AudioFalloff GetFalloff(AudioEmitterHandle emitter) const;

    // Number of currently-alive voices (pending or playing). The voice-level
    // observable for tests and debug overlays; per-voice handles stay private
    // to their owners (e.g. AudioEmitterSystem's pimpl).
    std::size_t GetAliveVoiceCount() const;

    // Bus volume (linear 0..1). Clamped in SetBusVolume.
    void SetBusVolume(AudioBusId bus, float linearVolume);
    float GetBusVolume(AudioBusId bus) const;

    /**
     * Silence the speakers without touching voices, bus volumes or the output capture.
     *
     * While muted the device receives silence: every voice keeps playing and advancing,
     * one-shots still finish and are reaped on time, and SetBusVolume/GetBusVolume (the
     * Master bus included) keep their values. The output capture callback still receives
     * the full mix, so a recording keeps its sound. The Master bus meter reads what reaches
     * the speakers (zero while muted); the other bus meters keep metering their buses.
     * The state is held here, so muting before Initialize() is honoured once the device
     * starts, and it survives Shutdown()/Initialize().
     */
    void SetMasterMuted(bool muted);
    /** True while SetMasterMuted(true) is in effect. */
    bool IsMasterMuted() const { return m_MasterMuted; }

    /** Per-bus meter level (0..1) for VU display. Real metering: backend updates from audio; placeholder: peaks at volume and decays. */
    float GetBusMeterLevel(AudioBusId bus) const;

    /** Approximate hardware output buffer latency in milliseconds (period frames / sample rate). 0 if unavailable. */
    float GetOutputLatencyMs() const;

    /** Capture final mixed output PCM from the audio device callback. Pass nullptr to disable. */
    void SetOutputCaptureCallback(OutputCaptureCallback callback);

private:
    class MiniaudioBackend;
    struct Impl;

    AssetManager* m_Assets = nullptr; // not owned
    std::unique_ptr<MiniaudioBackend> m_Backend;
    std::unique_ptr<Impl> m_Impl;
    AudioSystemConfig m_Config{};
    bool m_Initialized = false;
    bool m_MasterMuted = false;
};

} // namespace GameEngine::Audio

