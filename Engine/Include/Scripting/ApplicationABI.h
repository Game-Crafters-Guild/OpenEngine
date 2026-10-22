#pragma once

#include "Scripting/ScriptingABI.h"

#ifdef __cplusplus
extern "C" {
#endif

GE_API GE_Result GE_CDECL GE_Application_RequestExit(int32_t exitCode);

#ifdef __cplusplus
}
#endif
