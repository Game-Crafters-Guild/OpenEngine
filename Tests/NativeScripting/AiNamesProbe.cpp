// A user module that registers AI names with the GameSDK macros. The GE_PROBE_DROPPED build stands in for a
// newer version of the module's code that registers only the condition.
#include "GameSDK/AiNames.h"
#include "NativeScripting/NativeScriptingABI.h"

#include <algorithm>
#include <type_traits>

namespace
{
using namespace GameEngine;

void ReadThreat(const ECS::World&, std::span<const ECS::EntityHandle>, std::span<float32> out)
{
    std::fill(out.begin(), out.end(), 0.0f);
}

AIECS::AiTaskStatus TickWalk(AIECS::AiTaskContext&)
{
    return AIECS::AiTaskStatus::Running;
}

void SeesEnemy(const ECS::World&, std::span<const ECS::EntityHandle>, std::span<uint8> out)
{
    std::fill(out.begin(), out.end(), uint8{0});
}
} // namespace

GE_REGISTER_AI_CONDITION(SeesEnemy, "ProbeSeesEnemy")
#if !defined(GE_PROBE_DROPPED)
GE_REGISTER_AI_INPUT(ReadThreat, JobThreads, "ProbeThreat", "Threat level", "m")
GE_REGISTER_AI_TASK(nullptr, TickWalk, nullptr, "ProbeWalk")
GE_REGISTER_AI_EVENT(kProbeAlarm, "ProbeAlarm")
static_assert(std::is_same_v<decltype(kProbeAlarm), const GameEngine::uint16>, "the event macro defines the id");
#endif

extern "C"
{
GE_USERAPI uint32_t GE_UserModule_AbiVersion_v1() { return GE_USERMODULE_ABI_VERSION; }

GE_USERAPI void GE_UserModule_ToolchainFingerprint_v1(GE_ToolchainFingerprint* out)
{
    GE_FillToolchainFingerprint(out);
}

GE_USERAPI int GE_UserModule_Register_v1(const GE_UserModuleHostApi*) { return 0; }

GE_USERAPI void GE_UserModule_Unregister_v1(const GE_UserModuleHostApi*) {}
}
