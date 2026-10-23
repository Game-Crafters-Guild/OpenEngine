#include "ScriptThunks.h"

#include "AbiTextOut.h"
#include "Core/Engine.h"
#include "Logger/Logger.h"

#include <cstdio>
#include <mutex>
#include <set>
#include <string>

namespace GameEngine::ScriptingAbi
{
namespace
{

std::string& LastErrorText()
{
    thread_local std::string s_Text;
    return s_Text;
}

GE_Result Report(GE_Result code, std::string text)
{
    LastErrorText() = std::move(text);
    return code;
}

// True the first time `function` is refused off its thread in this process: a script that calls
// from its own thread every frame would otherwise fill the log with one line.
bool FirstThreadRefusal(const char* function)
{
    static std::mutex s_Mutex;
    static std::set<std::string> s_Reported;
    std::lock_guard<std::mutex> lock(s_Mutex);
    return s_Reported.insert(function).second;
}

std::string Hex(std::uint64_t value)
{
    char text[19];
    std::snprintf(text, sizeof(text), "0x%016llx", static_cast<unsigned long long>(value));
    return text;
}

} // namespace

GE_Result RefuseNull(const char* function, const char* parameter)
{
    return Report(GE_Result_InvalidArg, std::string(function) + ": `" + parameter + "` is a null pointer");
}

GE_Result RefuseNegativeLength(const char* function, const char* parameter)
{
    return Report(GE_Result_InvalidArg, std::string(function) + ": `" + parameter + "` is negative. Pass a byte count of 0 or more");
}

GE_Result RefuseZeroId(const char* function, const char* parameter)
{
    return Report(GE_Result_InvalidArg,
                  std::string(function) + ": `" + parameter + "` is 0, which names no id. Pass the id of a name");
}

GE_Result RefuseNoEntity(const char* function, const char* parameter)
{
    return Report(GE_Result_InvalidArg, std::string(function) + ": `" + parameter +
                                            "` is Entity.None, which names no entity. Pass the entity "
                                            "whose events you want");
}

GE_Result RefuseThread(const char* function, ThreadRule rule)
{
    const std::string text =
        rule == ThreadRule::Main
            ? std::string(function) + " was called off the main thread and was refused: it changes or reads state "
                                      "the main thread owns. Call it from a system's update, or post the call to "
                                      "the main thread with MainThread.Post"
            : std::string(function) + " was called from a thread the engine does not run and was refused: the "
                                      "engine's own threads can write what it reads while it runs. Call it from a "
                                      "system's update, or post the call to the main thread with MainThread.Post";
    if (FirstThreadRefusal(function))
        Logger::Log::Error("{}", text);
    return Report(GE_Result_Fail, text);
}

GE_Result CallFailed(const char* function, const char* what)
{
    return Report(GE_Result_Fail, std::string(function) + " failed: " + what);
}

GE_Result ServeTable(const char* module, std::uint64_t expectedHash, std::uint64_t requestedHash, const void* table,
                     std::uint32_t tableSize, const void** outTable, std::uint32_t* outSize)
{
    if (outTable == nullptr || outSize == nullptr)
        return RefuseNull((std::string("GE_") + module + "_GetInterface").c_str(), outTable == nullptr ? "outTable" : "outSize");
    *outTable = nullptr;
    *outSize = 0;
    if (requestedHash != expectedHash)
        return Report(GE_Result_InvalidArg,
                      std::string("The ") + module + " binding was built from the script API with hash " + Hex(requestedHash) +
                          ", and this engine serves " + Hex(expectedHash) +
                          ". If a *ScriptApi.h changed, build the UpdateEngineApi target and rebuild; otherwise rebuild the engine and the managed assemblies from one source tree");
    *outTable = table;
    *outSize = tableSize;
    return GE_Result_Ok;
}

std::string_view LastError()
{
    return LastErrorText();
}

} // namespace GameEngine::ScriptingAbi

extern "C" GE_API GE_Result GE_CDECL GE_GetLastScriptError(char* buffer, int32_t bufferLen, int32_t* outLen)
{
    return GameEngine::ScriptingAbi::CopyTextOut(GameEngine::ScriptingAbi::LastError(), buffer, bufferLen, outLen);
}
