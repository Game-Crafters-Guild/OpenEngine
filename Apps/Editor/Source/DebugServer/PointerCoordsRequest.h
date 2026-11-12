#pragma once

#include "Mathematics/Vector2.h"

#include <cstddef>
#include <vector>

#include <nlohmann/json.hpp>

namespace GameEngine::Editor
{

// Reads a UI-logical pointer coordinate pair out of a debug-server request — ("x","y")
// for click_element and move_pointer, ("startX","startY")/("endX","endY") for a drag.
//
// Every JSON number arrives as a double, so 1e300 survives parsing and becomes infinity
// in the narrowing cast to float; from there the router carries it into UI hit-testing,
// the editor InputSystem and — in play mode — gameplay. Injection re-checks the pair it
// is finally handed and drops it silently, which is the right answer for a coordinate a
// handler computed itself and the wrong one for a request field: rejected here, the
// value is still attributable to the field that carried it.
//
// On success writes x/y and leaves `error` untouched. On rejection returns false and
// fills `error` with the RefuseRequest refusal the handler returns verbatim; the message
// names both coordinate keys and echoes both RAW request values, so a caller sees the
// number it sent rather than the infinity it narrowed to.
bool ReadPointerCoords(const nlohmann::json& params, const char* xKey, const char* yKey, float& x, float& y,
                       nlohmann::json& error);

// Reads the optional "clickCount" of a click request: an integer from 1 to `maxCount`, 1 when
// absent. A fraction, a bool, a string, null and an integer out of range are refused, never
// narrowed: 2.5 or 4294967298 read as 2 would run a double-click the caller did not ask for.
// On success writes clickCount and leaves `error` untouched; on rejection returns false and
// fills `error` with the RefuseRequest refusal, which names the accepted values and echoes
// the raw request value.
bool ReadClickCount(const nlohmann::json& params, int maxCount, int& clickCount, nlohmann::json& error);

// Appends the points a drag walks after its start: the "path" points {"x","y"} in order, then
// "endX"/"endY", in UI-logical pixels. Refuses a request with no path point and no end, a
// malformed point, more than `maxPathPoints` path points or a coordinate out of float range.
bool ReadDragPath(const nlohmann::json& params, std::size_t maxPathPoints, std::vector<Mathematics::Vector2>& points,
                  nlohmann::json& error);

// The pointer position of each move of a drag along `points`, the start first. Each segment gets
// moves in proportion to its length, at least one, and its last move lands on its end point.
// Returns `steps` positions, or one per segment when there are more segments than steps.
std::vector<Mathematics::Vector2> SampleDragPath(const std::vector<Mathematics::Vector2>& points, int steps);

// Reads the "mods" field of an input request (send_key, send_scroll) as a GLFW modifier mask: one
// name, an array of names or a raw mask integer. Names ignore case: shift, control or ctrl, alt,
// super or cmd or meta. An unknown name adds nothing; an absent field is no modifier.
int ReadKeyModifiers(const nlohmann::json& params);

} // namespace GameEngine::Editor
