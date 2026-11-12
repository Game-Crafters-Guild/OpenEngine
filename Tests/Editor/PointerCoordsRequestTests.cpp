// The finite-coordinate gate every debug-server pointer request passes through, and
// the reply it refuses with.
//
// The predicate itself is covered by InjectedInputRoutingTests; what is pinned here is
// the request-shaped half nothing else reaches: which request field a rejection names,
// and that the reply echoes the RAW number the caller sent rather than the infinity it
// narrowed to. A rejection that said only "coordinates must be finite" would leave a
// caller of drag_element — four coordinate fields in one request — with no way to tell
// which one it got wrong.
//
// Requests are parsed from JSON text, the form they actually arrive in: 1e300 is an
// ordinary JSON number, and only the narrowing cast to float makes it infinite.
#include <gtest/gtest.h>

#include "DebugServer/DebugServerReply.h"
#include "DebugServer/PointerCoordsRequest.h"
#include "Input/InputSystem.h"

#include <cmath>
#include <string>
#include <utility>
#include <vector>

using namespace GameEngine;
using nlohmann::json;

namespace
{
std::string RejectionMessage(const json& params, const char* xKey, const char* yKey)
{
    float x = 0.0f;
    float y = 0.0f;
    json error;
    EXPECT_FALSE(Editor::ReadPointerCoords(params, xKey, yKey, x, y, error));
    EXPECT_TRUE(Editor::IsRefusal(error)) << error.dump();
    return Editor::HandlerResponse("0", error).value("error", std::string());
}
} // namespace

TEST(PointerCoordsRequestTests, FinitePairIsAcceptedAndLeavesTheErrorUntouched)
{
    const json params = json::parse(R"({"x": 120.5, "y": -60.25})");

    float x = 0.0f;
    float y = 0.0f;
    json error = json();

    EXPECT_TRUE(Editor::ReadPointerCoords(params, "x", "y", x, y, error));
    EXPECT_FLOAT_EQ(x, 120.5f);
    EXPECT_FLOAT_EQ(y, -60.25f);
    // The handlers return `error` verbatim on a false result and ignore it otherwise;
    // an acceptance that wrote to it would still be answered with, so it stays null.
    EXPECT_TRUE(error.is_null()) << error.dump();
}

// The float range's own edge stays acceptable: the gate rejects what cannot be
// represented, not what is merely large.
TEST(PointerCoordsRequestTests, LargeButRepresentableCoordinatesAreAccepted)
{
    const json params = json::parse(R"({"x": 3.0e38, "y": -3.0e38})");

    float x = 0.0f;
    float y = 0.0f;
    json error;

    EXPECT_TRUE(Editor::ReadPointerCoords(params, "x", "y", x, y, error));
    EXPECT_TRUE(std::isfinite(x));
    EXPECT_TRUE(std::isfinite(y));
}

TEST(PointerCoordsRequestTests, OutOfRangeXIsRejectedNamingTheFieldsAndEchoingTheRawValue)
{
    const json params = json::parse(R"({"x": 1e300, "y": 60})");
    const std::string message = RejectionMessage(params, "x", "y");

    EXPECT_NE(message.find("x/y must be finite numbers in float range"), std::string::npos) << message;
    // The raw request value, not the infinity it became — a reply reading "got x=inf"
    // describes the narrowing rather than the number the caller has to fix.
    EXPECT_NE(message.find("x=" + params["x"].dump()), std::string::npos) << message;
    EXPECT_NE(message.find("y=" + params["y"].dump()), std::string::npos) << message;
    EXPECT_EQ(message.find("inf"), std::string::npos) << message;
    EXPECT_NE(message.find("Pass UI-logical pixel coordinates."), std::string::npos) << message;
}

TEST(PointerCoordsRequestTests, OutOfRangeYIsRejectedNamingTheFieldsAndEchoingTheRawValue)
{
    const json params = json::parse(R"({"x": 120, "y": -1e300})");
    const std::string message = RejectionMessage(params, "x", "y");

    EXPECT_NE(message.find("x/y must be finite numbers in float range"), std::string::npos) << message;
    EXPECT_NE(message.find("x=" + params["x"].dump()), std::string::npos) << message;
    EXPECT_NE(message.find("y=" + params["y"].dump()), std::string::npos) << message;
    EXPECT_EQ(message.find("inf"), std::string::npos) << message;
}

TEST(PointerCoordsRequestTests, BothCoordinatesOutOfRangeAreBothEchoed)
{
    const json params = json::parse(R"({"x": 1e300, "y": -1e300})");
    const std::string message = RejectionMessage(params, "x", "y");

    EXPECT_NE(message.find("x=" + params["x"].dump()), std::string::npos) << message;
    EXPECT_NE(message.find("y=" + params["y"].dump()), std::string::npos) << message;
    EXPECT_EQ(message.find("inf"), std::string::npos) << message;
}

// drag_element reads two pairs out of one request. The reply has to name the pair it
// rejected, or the caller cannot tell a bad start from a bad end.
TEST(PointerCoordsRequestTests, RejectionNamesTheDragFieldsItWasAskedFor)
{
    const json params = json::parse(R"({"startX": 1e300, "startY": 10, "endX": 20, "endY": 1e300})");

    const std::string startMessage = RejectionMessage(params, "startX", "startY");
    EXPECT_NE(startMessage.find("startX/startY must be finite numbers in float range"), std::string::npos)
        << startMessage;
    EXPECT_NE(startMessage.find("startX=" + params["startX"].dump()), std::string::npos) << startMessage;
    EXPECT_EQ(startMessage.find("endX"), std::string::npos) << startMessage;

    const std::string endMessage = RejectionMessage(params, "endX", "endY");
    EXPECT_NE(endMessage.find("endX/endY must be finite numbers in float range"), std::string::npos) << endMessage;
    EXPECT_NE(endMessage.find("endY=" + params["endY"].dump()), std::string::npos) << endMessage;
    EXPECT_EQ(endMessage.find("startX"), std::string::npos) << endMessage;
}

namespace
{
constexpr int kMaxClickCount = 3;

std::string ClickCountRejection(const char* paramsText)
{
    int clickCount = -7;
    json error;
    EXPECT_FALSE(Editor::ReadClickCount(json::parse(paramsText), kMaxClickCount, clickCount, error)) << paramsText;
    EXPECT_EQ(clickCount, -7) << "a refused request leaves the count untouched";
    EXPECT_TRUE(Editor::IsRefusal(error)) << error.dump();
    return Editor::HandlerResponse("0", error).value("error", std::string());
}
} // namespace

TEST(PointerCoordsRequestTests, AnAbsentClickCountIsOneClick)
{
    int clickCount = 0;
    json error;
    EXPECT_TRUE(Editor::ReadClickCount(json::parse(R"({"x": 1, "y": 2})"), kMaxClickCount, clickCount, error));
    EXPECT_EQ(clickCount, 1);
    EXPECT_TRUE(error.is_null());
}

TEST(PointerCoordsRequestTests, AnIntegerClickCountInRangeIsAccepted)
{
    for (const char* text : {R"({"clickCount": 1})", R"({"clickCount": 2})", R"({"clickCount": 3})"})
    {
        int clickCount = 0;
        json error;
        EXPECT_TRUE(Editor::ReadClickCount(json::parse(text), kMaxClickCount, clickCount, error)) << text;
        EXPECT_EQ(clickCount, json::parse(text)["clickCount"].get<int>());
        EXPECT_TRUE(error.is_null());
    }
}

TEST(PointerCoordsRequestTests, AFractionalClickCountIsRefusedNotTruncated)
{
    const std::string message = ClickCountRejection(R"({"clickCount": 2.5})");
    EXPECT_NE(message.find("clickCount must be an integer from 1 to 3"), std::string::npos) << message;
    EXPECT_NE(message.find("2.5"), std::string::npos) << message;
}

TEST(PointerCoordsRequestTests, ABoolClickCountIsRefusedNotReadAsOne)
{
    const std::string message = ClickCountRejection(R"({"clickCount": true})");
    EXPECT_NE(message.find("got true"), std::string::npos) << message;
}

TEST(PointerCoordsRequestTests, AStringOrNullClickCountIsRefusedWithTheAcceptedValues)
{
    for (const char* text : {R"({"clickCount": "2"})", R"({"clickCount": null})"})
    {
        const std::string message = ClickCountRejection(text);
        EXPECT_NE(message.find("clickCount must be an integer from 1 to 3"), std::string::npos) << message;
        EXPECT_EQ(message.find("json.exception"), std::string::npos) << message;
    }
}

TEST(PointerCoordsRequestTests, AClickCountOutOfRangeIsRefusedBeforeItNarrows)
{
    // 4294967298 is 2^32 + 2: narrowed to a 32-bit int first, it would read as a double-click.
    for (const char* text : {R"({"clickCount": 0})", R"({"clickCount": -1})", R"({"clickCount": 4})",
                             R"({"clickCount": 4294967298})", R"({"clickCount": 18446744073709551615})"})
    {
        const std::string message = ClickCountRejection(text);
        EXPECT_NE(message.find(json::parse(text)["clickCount"].dump()), std::string::npos) << message;
    }
}

TEST(PointerCoordsRequestTests, AStraightDragMovesInEvenStepsToItsEnd)
{
    const std::vector<Mathematics::Vector2> moves =
        Editor::SampleDragPath({Mathematics::Vector2(0.0f, 10.0f), Mathematics::Vector2(100.0f, 10.0f)}, 4);

    ASSERT_EQ(moves.size(), 4u);
    for (std::size_t i = 0; i < moves.size(); ++i)
    {
        EXPECT_FLOAT_EQ(moves[i].x, 25.0f * static_cast<float>(i + 1)) << i;
        EXPECT_FLOAT_EQ(moves[i].y, 10.0f) << i;
    }
}

// A path's corners are the gesture: each segment takes moves by its length and ends on its point,
// so a stroke that comes back to its start ends exactly there.
TEST(PointerCoordsRequestTests, APathDragLandsOnEveryCornerAndSpreadsTheMovesByLength)
{
    const std::vector<Mathematics::Vector2> corners = {
        Mathematics::Vector2(0.0f, 0.0f), Mathematics::Vector2(300.0f, 0.0f), Mathematics::Vector2(300.0f, 100.0f),
        Mathematics::Vector2(0.0f, 100.0f), Mathematics::Vector2(0.0f, 0.0f)};
    const std::vector<Mathematics::Vector2> moves = Editor::SampleDragPath(corners, 16);

    ASSERT_EQ(moves.size(), 16u);
    // 800 px over 16 moves: 6 on each long side, 2 on each short side.
    for (const auto& [index, corner] : {std::pair{5, 1}, std::pair{7, 2}, std::pair{13, 3}, std::pair{15, 4}})
    {
        EXPECT_FLOAT_EQ(moves[index].x, corners[corner].x) << index;
        EXPECT_FLOAT_EQ(moves[index].y, corners[corner].y) << index;
    }
    EXPECT_FLOAT_EQ(moves[0].x, 50.0f);
    EXPECT_FLOAT_EQ(moves[6].y, 50.0f);

    // Fewer steps than segments still visits every corner.
    const std::vector<Mathematics::Vector2> few = Editor::SampleDragPath(corners, 2);
    ASSERT_EQ(few.size(), 4u);
    for (std::size_t i = 0; i < few.size(); ++i)
        EXPECT_EQ(few[i], corners[i + 1]) << i;
}

TEST(PointerCoordsRequestTests, ADragPathIsThePathPointsThenTheEnd)
{
    std::vector<Mathematics::Vector2> points;
    json error;
    ASSERT_TRUE(Editor::ReadDragPath(
        json::parse(R"({"path": [{"x": 1, "y": 2}, {"x": 3, "y": 4}], "endX": 5, "endY": 6})"), 8, points, error))
        << error.dump();
    ASSERT_EQ(points.size(), 3u);
    EXPECT_EQ(points[0], Mathematics::Vector2(1.0f, 2.0f));
    EXPECT_EQ(points[1], Mathematics::Vector2(3.0f, 4.0f));
    EXPECT_EQ(points[2], Mathematics::Vector2(5.0f, 6.0f));

    for (const char* text : {R"({})", R"({"path": []})", R"({"path": [{"x": 1}]})", R"({"path": [[1, 2]]})",
                             R"({"path": {"x": 1, "y": 2}})", R"({"path": [{"x": 1e300, "y": 2}]})",
                             R"({"path": [{"x": 1, "y": 2}, {"x": 1, "y": 2}, {"x": 1, "y": 2}]})"})
    {
        points.clear();
        error = json();
        EXPECT_FALSE(Editor::ReadDragPath(json::parse(text), 2, points, error)) << text;
        EXPECT_TRUE(Editor::IsRefusal(error)) << text << " " << error.dump();
    }

    // The handler puts the start in `points` first: an empty path with no end is still refused.
    points = {Mathematics::Vector2(1.0f, 2.0f)};
    error = json();
    EXPECT_FALSE(Editor::ReadDragPath(json::parse(R"({"path": []})"), 2, points, error));
    EXPECT_TRUE(Editor::IsRefusal(error)) << error.dump();
}

// "mods" is one name, an array of names or a raw mask; names ignore case and an unknown name adds nothing.
TEST(PointerCoordsRequestTests, KeyModifiersReadANameAnArrayOrAMask)
{
    EXPECT_EQ(Editor::ReadKeyModifiers(json::parse(R"({})")), 0);
    EXPECT_EQ(Editor::ReadKeyModifiers(json::parse(R"({"mods": "Ctrl"})")), Input::kModControl);
    EXPECT_EQ(Editor::ReadKeyModifiers(json::parse(R"({"mods": ["shift", "cmd", "hyper"]})")),
              Input::kModShift | Input::kModSuper);
    EXPECT_EQ(Editor::ReadKeyModifiers(json::parse(R"({"mods": 5})")), 5);
    EXPECT_EQ(Editor::ReadKeyModifiers(json::parse(R"({"mods": "hyper"})")), 0);
}
