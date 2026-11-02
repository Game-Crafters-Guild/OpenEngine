#include <gtest/gtest.h>

#include "DebugServer/SceneDegradedReport.h"

#include <cstddef>
#include <optional>
#include <string>

using GameEngine::Editor::BuildSceneDegradedReport;
using GameEngine::Editor::SceneLoadDegraded;
using GameEngine::Scene::SceneDegradation;

namespace
{

SceneDegradation::Skip MakeSkip(const char* component, const char* field, bool preserved)
{
    SceneDegradation::Skip s{};
    s.entityId = "lamp";
    s.component = component;
    s.field = field;
    s.message = "unknown enum value";
    s.line = 5;
    s.preserved = preserved;
    return s;
}

SceneLoadDegraded MakeRecord(std::initializer_list<SceneDegradation::Skip> skips)
{
    SceneLoadDegraded d{};
    d.DocumentPath = "C:/scenes/degraded.scene";
    for (const SceneDegradation::Skip& s : skips)
        d.Census.skips.push_back(s);
    return d;
}

} // namespace

// A clean load says nothing at all — a degraded-scene object on a healthy scene would teach every
// reader to ignore the field.
TEST(SceneDegradedReport, CleanLoadReportsNull)
{
    EXPECT_TRUE(BuildSceneDegradedReport(std::nullopt, std::nullopt, 0).is_null());
}

// The ordinary degraded case: both counts agree because nothing has retired yet.
TEST(SceneDegradedReport, FreshlyDegradedLoadReportsBothCountsAndTheRows)
{
    const SceneLoadDegraded atLoad = MakeRecord({MakeSkip("Light", "type", true)});
    const nlohmann::json r = BuildSceneDegradedReport(atLoad, atLoad, 0);

    EXPECT_EQ(r["loadTimeCount"], 1u);
    EXPECT_EQ(r["outstandingCount"], 1u);
    EXPECT_EQ(r["droppedCount"], 0u);
    ASSERT_EQ(r["skips"].size(), 1u);
    EXPECT_EQ(r["skips"][0]["component"], "Light");
    EXPECT_EQ(r["skips"][0]["field"], "type");
    EXPECT_EQ(r["skips"][0]["preserved"], true);
}

// THE RECONCILIATION. Once the user has discarded every override there is nothing outstanding, but
// the load really was partial and the window title still says so. Going null here would have an
// agent read a healthy scene while a human reads a partial one — two readers of one editor, unable
// to agree. Presence follows the load-time record, exactly like the title.
TEST(SceneDegradedReport, StaysPresentWhenNothingIsOutstandingSoItCannotContradictTheTitle)
{
    const SceneLoadDegraded atLoad = MakeRecord({MakeSkip("Light", "type", true)});
    const nlohmann::json r = BuildSceneDegradedReport(atLoad, std::nullopt, 0);

    ASSERT_FALSE(r.is_null())
        << "the load-time record still stands, so this must not call the scene healthy";
    EXPECT_EQ(r["loadTimeCount"], 1u) << "what the load could not apply does not change";
    EXPECT_EQ(r["outstandingCount"], 0u) << "and what a save must answer for is now nothing";
    EXPECT_TRUE(r["skips"].empty())
        << "a discarded override must never be listed as text a save will write back";
}

// The rows come from the OUTSTANDING record, not the load-time one, so a retired override drops out
// of the list even while the object itself stays present.
TEST(SceneDegradedReport, RowsComeFromTheOutstandingRecordNotTheLoadTimeOne)
{
    const SceneLoadDegraded atLoad =
        MakeRecord({MakeSkip("Light", "type", true), MakeSkip("SplineFence", "SpanGrade", true)});
    const SceneLoadDegraded outstanding = MakeRecord({MakeSkip("Light", "type", true)});

    const nlohmann::json r = BuildSceneDegradedReport(atLoad, outstanding, 0);

    EXPECT_EQ(r["loadTimeCount"], 2u);
    EXPECT_EQ(r["outstandingCount"], 1u);
    ASSERT_EQ(r["skips"].size(), 1u);
    EXPECT_EQ(r["skips"][0]["component"], "Light")
        << "the retired row is gone from the list, not merely re-flagged";
}

// droppedCount is the never-preserved subset of the outstanding record: what a save drops now.
TEST(SceneDegradedReport, DroppedCountTracksTheNeverPreservedRows)
{
    const SceneLoadDegraded atLoad =
        MakeRecord({MakeSkip("Light", "type", true), MakeSkip("Ghost", "field", false)});
    const SceneLoadDegraded outstanding = MakeRecord({MakeSkip("Ghost", "field", false)});

    const nlohmann::json r = BuildSceneDegradedReport(atLoad, outstanding, 0);

    EXPECT_EQ(r["droppedCount"], 1u);
    EXPECT_EQ(r["loadTimeCount"], 2u);
    EXPECT_EQ(r["outstandingCount"], 1u);
    ASSERT_EQ(r["skips"].size(), 1u);
    EXPECT_EQ(r["skips"][0]["preserved"], false);
}

// A scene with tens of thousands of stale lines must not make every get_editor_state reply carry
// all of them: the reply stays small and the counts still state the whole.
TEST(SceneDegradedReport, ReplySizeStaysBoundedForAFiftyThousandRowScene)
{
    constexpr std::size_t kRowCount = 50000;
    SceneLoadDegraded record{};
    record.DocumentPath = "C:/scenes/degraded.scene";
    record.Census.skips.reserve(kRowCount);
    for (std::size_t i = 0; i < kRowCount; ++i)
        record.Census.skips.push_back(MakeSkip("SplineFence", "SpanGrade", true));

    const nlohmann::json r = BuildSceneDegradedReport(record, record, 0);

    constexpr std::size_t kMaxReplyBytes = 64 * 1024;
    EXPECT_LT(r.dump().size(), kMaxReplyBytes);
    EXPECT_EQ(r["outstandingCount"], kRowCount);
    EXPECT_EQ(r["loadTimeCount"], kRowCount);
    EXPECT_EQ(r["skips"].size(), GameEngine::Editor::kSceneDegradedSkipsPerPage);
}

// Paging reaches every row: the last page holds the remainder and an offset past the end holds none.
TEST(SceneDegradedReport, SkipsOffsetSelectsThePageAndThePageAfterTheLastIsEmpty)
{
    constexpr std::size_t kPage = GameEngine::Editor::kSceneDegradedSkipsPerPage;
    constexpr std::size_t kRowCount = kPage + 3;
    SceneLoadDegraded record{};
    for (std::size_t i = 0; i < kRowCount; ++i)
        record.Census.skips.push_back(MakeSkip("Light", i + 1 == kRowCount ? "last" : "type", true));

    const nlohmann::json lastPage = BuildSceneDegradedReport(record, record, kPage);
    EXPECT_EQ(lastPage["skipsOffset"], kPage);
    ASSERT_EQ(lastPage["skips"].size(), 3u);
    EXPECT_EQ(lastPage["skips"][2]["field"], "last");

    const nlohmann::json pastTheEnd = BuildSceneDegradedReport(record, record, kRowCount + 10);
    EXPECT_TRUE(pastTheEnd["skips"].empty());
    EXPECT_EQ(pastTheEnd["outstandingCount"], kRowCount);
}

// A never-preserved row retires with its entity or component, and the dropped count follows it.
TEST(SceneDegradedReport, DroppedCountFollowsTheOutstandingRecord)
{
    const SceneLoadDegraded atLoad =
        MakeRecord({MakeSkip("Light", "type", true), MakeSkip("Ghost", "field", false)});
    const SceneLoadDegraded outstanding = MakeRecord({MakeSkip("Light", "type", true)});

    const nlohmann::json r = BuildSceneDegradedReport(atLoad, outstanding, 0);

    EXPECT_EQ(r["droppedCount"], 0u);
    EXPECT_EQ(r["loadTimeCount"], 2u);
}
