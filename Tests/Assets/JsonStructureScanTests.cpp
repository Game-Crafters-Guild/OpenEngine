// The scan that finds what a parsed JSON document cannot show: a key written twice in one object,
// and objects and arrays nested past a depth limit.

#include <gtest/gtest.h>

#include "Serialization/JsonRead.h"

#include <string>

using GameEngine::Serialization::JsonStructureScan;
using GameEngine::Serialization::ScanJsonStructure;

namespace
{

// `levels` arrays, one inside the next, around nothing.
std::string NestedArrays(size_t levels)
{
    return std::string(levels, '[') + std::string(levels, ']');
}

} // namespace

TEST(JsonStructureScan, FindsAKeyWrittenTwiceInOneObject)
{
    const JsonStructureScan scan = ScanJsonStructure(R"({"a": 1, "b": {"c": 2, "c": 3}})");
    EXPECT_EQ(scan.Found, JsonStructureScan::Finding::RepeatedKey);
    EXPECT_EQ(scan.Key, "c");
}

TEST(JsonStructureScan, FindsARepeatedKeyAfterAnInnerObjectCloses)
{
    const JsonStructureScan scan = ScanJsonStructure(R"({"a": {"b": 1}, "a": 2})");
    EXPECT_EQ(scan.Found, JsonStructureScan::Finding::RepeatedKey);
    EXPECT_EQ(scan.Key, "a");
}

// Each object has keys of its own: the same key in sibling objects, or in an object and the one
// that holds it, is not a repeat.
TEST(JsonStructureScan, AcceptsTheSameKeyInDifferentObjects)
{
    const JsonStructureScan siblings =
        ScanJsonStructure(R"([{"frame": 1, "name": "a"}, {"frame": 2, "name": "b"}, {"frame": 3}])");
    EXPECT_EQ(siblings.Found, JsonStructureScan::Finding::None);

    const JsonStructureScan nested = ScanJsonStructure(R"({"a": {"a": {"a": 1}}, "b": {"a": 2}})");
    EXPECT_EQ(nested.Found, JsonStructureScan::Finding::None);
}

TEST(JsonStructureScan, RefusesNestingPastTheDepthLimit)
{
    EXPECT_EQ(ScanJsonStructure(NestedArrays(4), 4).Found, JsonStructureScan::Finding::None);
    EXPECT_EQ(ScanJsonStructure(NestedArrays(5), 4).Found, JsonStructureScan::Finding::TooDeep);
    EXPECT_EQ(ScanJsonStructure(R"({"a": {"b": {}}})", 2).Found, JsonStructureScan::Finding::TooDeep);
    EXPECT_EQ(ScanJsonStructure(NestedArrays(200)).Found, JsonStructureScan::Finding::None);
}

TEST(JsonStructureScan, ReportsTextThatIsNotJson)
{
    EXPECT_EQ(ScanJsonStructure(R"({"a": 1,})").Found, JsonStructureScan::Finding::NotJson);
    EXPECT_EQ(ScanJsonStructure("").Found, JsonStructureScan::Finding::NotJson);
}
