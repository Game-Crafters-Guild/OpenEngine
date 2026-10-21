#pragma once

// The context of the binding generators' fixture script API: what its functions record, and the
// ScriptContext specialization its generated thunks call.

#include "Probe/ProbeScriptApi.h"
#include "Scripting/ScriptThunks.h"

#include <atomic>

namespace GameEngine::Probe
{

class ProbeSystem
{
  public:
    // The calls that reached a facade body, on any thread.
    static std::atomic<std::uint32_t>& Calls()
    {
        static std::atomic<std::uint32_t> s_Calls{0};
        return s_Calls;
    }

    // The system the thunks reach, or null for "not running".
    static ProbeSystem*& Current()
    {
        static ProbeSystem* s_Current = nullptr;
        return s_Current;
    }

    StringId LastSound = 0;
    SoundOptions LastOptions{};
    float LastGain = 0.0f;
    Loudness LastLoudness = Loudness::Normal;
    std::uint32_t LastLayers = 0;
    bool Flag = false;
    std::int8_t Level = 0;
    double Weight = 0.0;
    Mathematics::Quaternion Facing{};
    Mathematics::Matrix4x4 Pose{};
    ECS::EntityHandle LastEntity{};
    Mathematics::Vector3 LastForce{};
    float Speed = 0.0f;
    ECS::EntityHandle Nearest{};

    // The one Touched subscription the probe holds; its id, or 0 for none.
    struct TouchSubscription
    {
        SubscriptionId Id = 0;
        ECS::EntityHandle Scope{};
        ScriptCallback<Touch> Callback{};
    };

    static TouchSubscription& Touched()
    {
        static TouchSubscription s_Touched;
        return s_Touched;
    }
};

} // namespace GameEngine::Probe

namespace GameEngine::ScriptingAbi
{
template <>
inline Probe::ProbeSystem* ScriptContext<Probe::ProbeSystem>(EngineCore&)
{
    return Probe::ProbeSystem::Current();
}
} // namespace GameEngine::ScriptingAbi
