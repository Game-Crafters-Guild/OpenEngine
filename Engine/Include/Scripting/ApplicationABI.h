#pragma once

#include "Scripting/ScriptingABI.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Request game exit through the current application's host policy.
 *
 * The Player finishes its frame and shuts down with exitCode. The Editor queues
 * its normal Stop Play flow, keeping the editor and its unsaved documents open.
 * Calls must come from the main thread outside a job.
 * @return Ok on acceptance, NotInitialized with no application, or Fail when
 * the calling thread is refused or the host cannot handle the request.
 */
GE_API GE_Result GE_CDECL GE_Application_RequestExit(int32_t exitCode);

#ifdef __cplusplus
}
#endif
