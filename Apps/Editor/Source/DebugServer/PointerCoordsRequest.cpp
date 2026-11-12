#include "DebugServer/PointerCoordsRequest.h"

#include "DebugServer/DebugServerReply.h"
#include "DebugServer/InjectedInput.h"
#include "Input/InputSystem.h"
#include "Mathematics/Geometry.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

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

bool ReadDragPath(const nlohmann::json& params, std::size_t maxPathPoints, std::vector<Mathematics::Vector2>& points,
                  nlohmann::json& error)
{
    const auto path = params.find("path");
    const bool hasEnd = params.contains("endX") && params.contains("endY");
    if ((path == params.end() || path->empty()) && !hasEnd)
    {
        error = RefuseRequest("Provide path with at least one point, or endX/endY coordinates");
        return false;
    }
    if (path != params.end())
    {
        if (!path->is_array() || path->size() > maxPathPoints)
        {
            error = RefuseRequest("path must be an array of at most " + std::to_string(maxPathPoints) +
                                  " points {\"x\": number, \"y\": number}");
            return false;
        }
        for (std::size_t pointIndex = 0; pointIndex < path->size(); ++pointIndex)
        {
            const nlohmann::json& point = (*path)[pointIndex];
            if (!point.is_object() || !point.contains("x") || !point.contains("y") || !point["x"].is_number() ||
                !point["y"].is_number())
            {
                error = RefuseRequest("path[" + std::to_string(pointIndex) +
                                      "] must be a point {\"x\": number, \"y\": number} (got " + point.dump() + ")");
                return false;
            }
            Mathematics::Vector2 position;
            if (!ReadPointerCoords(point, "x", "y", position.x, position.y, error))
                return false;
            points.push_back(position);
        }
    }
    if (hasEnd)
    {
        Mathematics::Vector2 end;
        if (!ReadPointerCoords(params, "endX", "endY", end.x, end.y, error))
            return false;
        points.push_back(end);
    }
    return true;
}

std::vector<Mathematics::Vector2> SampleDragPath(const std::vector<Mathematics::Vector2>& points, int steps)
{
    std::vector<Mathematics::Vector2> moves;
    if (points.size() < 2)
        return moves;

    const int segmentCount = static_cast<int>(points.size()) - 1;
    const int moveCount = std::max(steps, segmentCount);
    std::vector<float> lengthToEnd(static_cast<std::size_t>(segmentCount));
    float totalLength = 0.0f;
    for (int segment = 0; segment < segmentCount; ++segment)
    {
        const std::size_t from = static_cast<std::size_t>(segment);
        totalLength += (points[from + 1] - points[from]).Length();
        lengthToEnd[static_cast<std::size_t>(segment)] = totalLength;
    }

    moves.reserve(static_cast<std::size_t>(moveCount));
    int lastMove = 0;
    for (int segment = 0; segment < segmentCount; ++segment)
    {
        // The move that lands on this segment's end: its share of the length, at least one move
        // after the previous segment, and one move left for each later segment.
        const float share = totalLength > 0.0f ? lengthToEnd[static_cast<std::size_t>(segment)] / totalLength
                                               : static_cast<float>(segment + 1) / static_cast<float>(segmentCount);
        const int endMove = std::clamp(static_cast<int>(std::lround(share * static_cast<float>(moveCount))),
                                       lastMove + 1, moveCount - (segmentCount - 1 - segment));
        const Mathematics::Vector2 from = points[static_cast<std::size_t>(segment)];
        const Mathematics::Vector2 to = points[static_cast<std::size_t>(segment) + 1];
        for (int move = lastMove + 1; move <= endMove; ++move)
        {
            const float t = static_cast<float>(move - lastMove) / static_cast<float>(endMove - lastMove);
            moves.push_back(move == endMove ? to : Mathematics::Lerp(from, to, t));
        }
        lastMove = endMove;
    }
    return moves;
}

namespace
{
int ModifierForName(std::string_view name)
{
    const std::string lowered = ToLowerAscii(name);
    if (lowered == "shift")
        return Input::kModShift;
    if (lowered == "control" || lowered == "ctrl")
        return Input::kModControl;
    if (lowered == "alt")
        return Input::kModAlt;
    if (lowered == "super" || lowered == "cmd" || lowered == "meta")
        return Input::kModSuper;
    return 0;
}
} // namespace

int ReadKeyModifiers(const nlohmann::json& params)
{
    const auto it = params.find("mods");
    if (it == params.end())
        return 0;
    if (it->is_number_integer())
        return it->get<int>();
    if (it->is_string())
        return ModifierForName(it->get<std::string>());
    int mods = 0;
    if (it->is_array())
    {
        for (const auto& entry : *it)
        {
            if (entry.is_string())
                mods |= ModifierForName(entry.get<std::string>());
        }
    }
    return mods;
}

} // namespace GameEngine::Editor
