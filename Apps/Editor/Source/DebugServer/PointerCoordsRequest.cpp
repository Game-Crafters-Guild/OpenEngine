#include "DebugServer/PointerCoordsRequest.h"

#include "DebugServer/DebugServerReply.h"
#include "DebugServer/InjectedInput.h"

#include <cstdint>
#include <string>

namespace GameEngine::Editor
{

bool ReadPointerCoords(const nlohmann::json& params, const char* xKey, const char* yKey, float& x, float& y,
                       nlohmann::json& error)
{
    x = params[xKey].get<float>();
    y = params[yKey].get<float>();
    if (IsInjectablePointerPosition(x, y))
        return true;

    error = RefuseRequest(std::string(xKey) + "/" + yKey +
                              " must be finite numbers in float range (got " + xKey + "=" +
                              params[xKey].dump() + ", " + yKey + "=" + params[yKey].dump() +
                              "). Pass UI-logical pixel coordinates.");
    return false;
}

bool ReadClickCount(const nlohmann::json& params, int maxCount, int& clickCount, nlohmann::json& error)
{
    const auto it = params.find("clickCount");
    if (it == params.end())
    {
        clickCount = 1;
        return true;
    }
    if (it->is_number_integer())
    {
        // Compared before narrowing to int. An unsigned value above the int64 range reads as
        // negative here, so it is refused like any other value out of range.
        const int64_t value = it->get<int64_t>();
        if (value >= 1 && value <= maxCount)
        {
            clickCount = static_cast<int>(value);
            return true;
        }
    }
    error = RefuseRequest("clickCount must be an integer from 1 to " + std::to_string(maxCount) + " (got " +
                          it->dump() + ").");
    return false;
}

} // namespace GameEngine::Editor
