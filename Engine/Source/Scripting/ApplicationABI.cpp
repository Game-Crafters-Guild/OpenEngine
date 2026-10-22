#include "Scripting/ApplicationABI.h"

#include "AbiMainThread.h"
#include "Core/Application.h"

GE_API GE_Result GE_CDECL GE_Application_RequestExit(int32_t exitCode)
{
    try
    {
        if (!GameEngine::ScriptingAbi::OnEngineMainThread())
            return GE_Result_Fail;

        auto* application = GameEngine::Application::Get();
        if (!application)
            return GE_Result_NotInitialized;

        application->RequestGameExit(exitCode);
        return GE_Result_Ok;
    }
    catch (...)
    {
        return GE_Result_Fail;
    }
}
