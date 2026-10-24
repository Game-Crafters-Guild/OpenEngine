#pragma once

// ScriptThunks.h - What the generated script API thunks call. Each module's
// <Module>.script.gen.cpp (written by Tools/ComponentScanner, NativeBindingEmitter) holds one
// thunk per function of the module's script API; the thunk looks the engine up once
// (EngineCore::GetInstance, a call into Engine), hands it to the bootstrap, the thread rule and
// ScriptContext, reports every refusal through the functions below, which set the calling
// thread's last error, and serves its table through ServeTable. A thunk with text reads it with
// TextIn and writes it with CopyTextOut (AbiTextOut.h).

#include "AbiMainThread.h"
#include "AbiTextOut.h"
#include "DllEngineBootstrap.h"
#include "PhysicsECS/PhysicsWorldService.h"
#include "Scripting/ScriptingABI.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace GameEngine::ScriptingAbi
{

template <class Context>
inline constexpr bool kScriptContextUnknown = true;

/**
 * @brief The running system a facade function's @ge-context parameter names, or null when the engine
 * has none (no runtime input yet, audio switched off). One specialization per context type; a thunk
 * for a context type without one does not compile.
 */
template <class Context>
Context* ScriptContext(EngineCore& engine)
{
    (void)engine;
    static_assert(!kScriptContextUnknown<Context>,
                  "A @ge-context type with no ScriptContext specialization: add one to Scripting/ScriptThunks.h "
                  "that returns the running system from EngineCore");
    return nullptr;
}

/// The input sink scripts read: the game's own InputSystem in the editor, the application's elsewhere.
template <>
inline Input::InputSystem* ScriptContext<Input::InputSystem>(EngineCore& engine)
{
    return engine.GetRuntimeInput();
}

/// The scene's ECS world: the engine's primary world, which the editor plays in and the Player runs.
template <>
inline ECS::World* ScriptContext<ECS::World>(EngineCore& engine)
{
    return engine.GetPrimaryWorld();
}

/// The physics world, which exists in play mode only.
template <>
inline Physics::PhysicsWorld* ScriptContext<Physics::PhysicsWorld>(EngineCore& engine)
{
    (void)engine;
    return PhysicsECS::PhysicsWorldService::TryGet();
}

/// The thread rule a refused call broke, for the last error and the log.
enum class ThreadRule : std::uint8_t
{
    Main,
    Engine,
};

/// Refuses a null pointer argument (`parameter` is its name, or "outResult" for the return value).
GE_Result RefuseNull(const char* function, const char* parameter);

/// Refuses a negative text length or buffer capacity (`parameter` is its name).
GE_Result RefuseNegativeLength(const char* function, const char* parameter);

/// The text of a (pointer, length) UTF-8 argument that the thunk has checked: a null pointer with length 0 is the
/// empty text. The view is the caller's, valid for the call only.
inline std::string_view TextIn(const char* text, std::int32_t length)
{
    return length == 0 ? std::string_view() : std::string_view(text, static_cast<std::size_t>(length));
}

/// Refuses a StringId argument of 0, which names no id.
GE_Result RefuseZeroId(const char* function, const char* parameter);

/// Refuses an entity argument that names no entity (an event's scope).
GE_Result RefuseNoEntity(const char* function, const char* parameter);

/// Refuses a call off the function's thread; logs once per process for each function.
GE_Result RefuseThread(const char* function, ThreadRule rule);

/**
 * @brief Answers a call while the engine is not running: GE_Result_NotInitialized, the out values left at their
 * defaults. An answer, not a refusal: it sets no last error, so a script that polls every frame before play pays
 * nothing for it.
 */
inline GE_Result EngineNotRunning()
{
    return GE_Result_NotInitialized;
}

/// Answers a call while the function's system is absent, as EngineNotRunning does.
inline GE_Result ContextMissing()
{
    return GE_Result_NotInitialized;
}

/// Answers a call whose facade function threw: GE_Result_Fail.
GE_Result CallFailed(const char* function, const char* what);

/// Refuses a script's write of a component the engine owns with GE_Result_InvalidArg; `text` names the component,
/// the system that writes it and the fix.
GE_Result RefuseScriptWrite(std::string text);

/**
 * @brief Serves a module table to a caller built from the same description: `expectedHash` is the table's,
 * `requestedHash` the caller's. A mismatch is refused with GE_Result_InvalidArg, a null table and a last
 * error that names both hashes and the fix.
 */
GE_Result ServeTable(const char* module, std::uint64_t expectedHash, std::uint64_t requestedHash, const void* table,
                     std::uint32_t tableSize, const void** outTable, std::uint32_t* outSize);

/// The calling thread's last error: the text of its most recent refused or failed script API call.
std::string_view LastError();

} // namespace GameEngine::ScriptingAbi
