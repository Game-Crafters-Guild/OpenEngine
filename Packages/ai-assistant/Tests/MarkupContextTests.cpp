// The AI Assistant's mark-up context: the `@markup` tokens, the block a mark-up becomes and its
// caps, what a turn sends, the "Ask the AI Assistant" prefill and the "Comment on mark-up" reply
// action. Mark-ups are read through the EditorSDK seam, which each test installs with handlers
// that answer as the editor's markup_* methods do; uninstalled before it ends.

#include "AgentConversationRowModel.h"
#include "AgentReplyActions.h"
#include "AiAssistantPanelService.h"
#include "AssistantActionLedger.h"
#include "AssistantTurnUndo.h"
#include "Conversation.h"
#include "MarkupActions.h"
#include "MarkupContext.h"
#include "MarkupSearchProvider.h"
#include "Providers/ClaudeApiProvider.h"

#include "Components/Markup/Markup.h"
#include "Components/Name.h"
#include "Components/SceneEntityTag.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Editor/Registries/EditorMarkupRequests.h"
#include "Editor/Registries/MarkupActionRegistry.h"
#include "MarkupECS/MarkupService.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <any>
#include <clocale>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace GameEngine;
using nlohmann::json;

namespace
{
using Answer = Editor::MarkupRequestAnswer;

struct UninstallGuard
{
    ~UninstallGuard() { Editor::EditorMarkupRequests::Get().Install({}); }
};

// A markup_list row whose tags are `tag` alone (none when empty). Its `tag`, the entity's scene tag,
// is a stand-in that no surface may show or name a mark-up by.
json Row(uint32_t id, const std::string& title, const std::string& tag, const std::string& kind)
{
    return json{{"entityId", id},
                {"tag", "scene-" + std::to_string(id)},
                {"title", title},
                {"tags", tag.empty() ? json::array() : json::array({tag})},
                {"kind", kind},
                {"status", "Requested"}};
}

json Entry(const std::string& author, const std::string& text)
{
    return json{{"kind", text.empty() ? "created" : "comment"}, {"author", author},
                {"time", "2026-10-13T09:00:00Z"}, {"text", text}};
}

// markup_get's "markup" for a box.
json Village()
{
    json markup = Row(7, "Village", "", "volume");
    markup["description"] = "Market square & well";
    markup["entries"] = json::array({Entry("Agent", ""), Entry("User", "Keep the well"),
                                     json{{"kind", "statusChange"}, {"author", "User"}, {"time", "2026-10-13T09:00:00Z"},
                                          {"status", "Requested"}, {"text", ""}}});
    markup["shape"] = json{{"shape", "box"},
                           {"center", {10.0, 0.0, 20.0}},
                           {"halfExtents", {5.0, 2.0, 5.0}},
                           {"rotation", {0.0, 0.0, 0.0, 1.0}}};
    return markup;
}

json Ring(const std::vector<std::pair<double, double>>& points)
{
    json ring = json::array();
    for (const auto& [x, z] : points)
        ring.push_back(json::array({x, z}));
    return ring;
}

std::vector<std::pair<double, double>> Circle(double radius, int count)
{
    std::vector<std::pair<double, double>> points;
    for (int i = 0; i < count; ++i)
    {
        const double angle = 6.283185307179586 * i / count;
        points.emplace_back(radius * std::cos(angle), radius * std::sin(angle));
    }
    return points;
}

// markup_get's "markup" for a region whose evaluated ring is `outline`.
json Meadow(const json& outline)
{
    json markup = Row(9, "Meadow", "meadow-1", "region");
    markup["entries"] = json::array({Entry("Agent", "")});
    markup["shape"] = json{{"shape", "region"},
                           {"extrudeHeight", 8.0},
                           {"knots", outline},
                           {"type", "linear"},
                           {"outline", outline},
                           {"area", 31415.9},
                           {"perimeter", 628.3},
                           {"bounds", {{"min", {-100.0, -100.0}}, {"max", {100.0, 100.0}}}},
                           {"members", json::array()}};
    return markup;
}

std::string Attribute(const std::string& block, const std::string& name)
{
    const std::string key = " " + name + "=\"";
    const std::size_t at = block.find(key);
    if (at == std::string::npos)
        return {};
    const std::size_t start = at + key.size();
    return block.substr(start, block.find('"', start) - start);
}

std::size_t PointCount(const std::string& points)
{
    return points.empty() ? 0 : static_cast<std::size_t>(std::count(points.begin(), points.end(), ' ')) + 1;
}

// A mark-up titled `title` in `world`, with notes and a scene tag of its own ("scene-<id>").
ECS::EntityHandle CreateMarkup(ECS::World& world, const char* title)
{
    const ECS::EntityHandle entity = world.CreateEntity();
    world.AddComponentImmediate(entity, Components::Markup{});
    Components::Name name{};
    std::strncpy(name.value, title, sizeof(name.value) - 1);
    world.AddComponentImmediate(entity, name);
    Components::SceneEntityTag sceneTag{};
    std::strncpy(sceneTag.value, ("scene-" + std::to_string(entity.id)).c_str(), sizeof(sceneTag.value) - 1);
    world.AddComponentImmediate(entity, sceneTag);
    EXPECT_TRUE(MarkupECS::MarkupService::Get().BeginMarkup(world, entity, Components::MarkupAuthor::User, 0));
    return entity;
}

// Installs a scene of `rows` whose markup_get answers `markups` by entity id, and records comments.
struct FakeScene
{
    json Rows = json::array();
    std::map<uint32_t, json> Markups;
    std::string ListRefusal;
    std::vector<json> Comments;

    void Install()
    {
        Editor::EditorMarkupRequests::Handlers handlers;
        handlers.List = [this](const json&)
        { return ListRefusal.empty() ? Answer{json{{"markups", Rows}}, {}} : Answer{{}, ListRefusal}; };
        handlers.Query = [this](const json& params)
        {
            const auto found = Markups.find(params["entityId"].get<uint32_t>());
            return found == Markups.end() ? Answer{{}, "No mark-up has that entityId"} : Answer{json{{"markup", found->second}}, {}};
        };
        handlers.Comment = [this](const json& params)
        {
            Comments.push_back(params);
            return Answer{json::object(), {}};
        };
        Editor::EditorMarkupRequests::Get().Install(std::move(handlers));
    }
};
} // namespace

// A token names a mark-up by its tag or its quoted title, after a space or at the start; a
// sentence's punctuation after a tag is not the tag's, an address is not a token, and a mark-up
// named twice is one token.
TEST(MarkupContext, TokensNameMarkupsByTagOrQuotedTitleOnceEach)
{
    const std::vector<MarkupToken> tokens = MarkupContext::FindTokens(
        "Widen @markup:road-2, then look at @markup:\"Village center\" and @markup:road-2 again; me@markup:x");
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].Tag, "road-2");
    EXPECT_EQ(tokens[0].Text, "@markup:road-2");
    EXPECT_EQ(tokens[1].Title, "Village center");
    EXPECT_EQ(tokens[1].Text, "@markup:\"Village center\"");
}

// A mark-up is named by its tag, the first of its tags, when no other mark-up's first tag is the
// same and the tag reads back from a prompt; else by its title when no other mark-up has it; else
// it is refused with its entity id and the fix.
TEST(MarkupContext, AMarkupIsNamedByItsTagThenItsTitleWhenEitherIsItsOwn)
{
    EXPECT_EQ(MarkupContext::TagOf(Row(3, "Village center", "v-1", "volume")), "v-1") << "not the scene tag";
    EXPECT_EQ(MarkupContext::NameOf({.EntityId = 3, .Title = "Village center", .Tag = "v-1"}).Token, "@markup:v-1");
    EXPECT_EQ(MarkupContext::NameOf({.EntityId = 3, .Title = "Village center"}).Token, "@markup:\"Village center\"");
    EXPECT_EQ(MarkupContext::NameOf({.EntityId = 3, .Title = "Village center", .Tag = "old town"}).Token,
              "@markup:\"Village center\"")
        << "a tag with a space is no token";
    EXPECT_EQ(MarkupContext::NameOf({.EntityId = 3, .Title = "Village center", .Tag = "v-1", .SameTag = 2}).Token,
              "@markup:\"Village center\"")
        << "a tag another mark-up has first";
    EXPECT_EQ(MarkupContext::NameOf({.EntityId = 3, .Title = "Meadow", .Tag = "grass", .SameTitle = 2, .SameTag = 3}).Refusal,
              "Its tag grass is the first tag of 3 mark-ups, and 2 mark-ups are titled Meadow: give this one (entity 3) "
              "a tag or a title of its own to name it");
}

// A title holding a quote or a backslash is named with \" and \\ in its token, which reads back to the
// same mark-up; a mark-up with neither a title nor a tag gets no token.
TEST(MarkupContext, ATitleWithAQuoteRoundTripsThroughItsToken)
{
    const json rows = json::array({Row(4, "Say \"hi\" \\ wave", "", "volume")});
    const std::string token = MarkupContext::NameOf({.EntityId = 4, .Title = "Say \"hi\" \\ wave"}).Token;
    EXPECT_EQ(token, "@markup:\"Say \\\"hi\\\" \\\\ wave\"");
    const std::vector<MarkupToken> tokens = MarkupContext::FindTokens("Look at " + token + " now");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].Title, "Say \"hi\" \\ wave");
    const json* row = MarkupContext::Resolve(tokens[0], rows);
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->value("entityId", 0u), 4u);
    EXPECT_EQ(MarkupContext::NameOf({.EntityId = 5}).Token, "");
    EXPECT_TRUE(MarkupContext::FindTokens("@markup:\"\" alone").empty());
}

// The `@` list: the scene's mark-ups whose title holds the query, any case, each carrying the token
// that names it; what was typed is kept for the prompt when the list closes without a choice.
TEST(MarkupContext, TheAtListFiltersByTitleOrTagAndCarriesEachToken)
{
    json meadow = Row(9, "Meadow", "meadow-1", "region");
    meadow["color"] = "#336699";
    MarkupSearchProvider search(
        json::array({Row(7, "Village", "", "volume"), meadow, Row(11, "", "dock-2", "path"), Row(12, "", "", "volume")}));
    std::vector<SearchResultItem> rows;
    const auto collect = [&rows](std::vector<SearchResultItem> batch, bool) { rows = std::move(batch); };
    search.BeginSearch("mEa", collect);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].Label, "Meadow");
    EXPECT_EQ(rows[0].Detail, "Requested \xC2\xB7 region \xC2\xB7 tag meadow-1");
    EXPECT_EQ(rows[0].Icon.SwatchArgb, 0xFF336699u) << "a swatch in the color the mark-up draws in";
    EXPECT_EQ(std::any_cast<MarkupPick>(rows[0].UserData).Token, "@markup:meadow-1");
    EXPECT_EQ(search.LastQuery(), "mEa");

    search.BeginSearch("DOCK", collect);
    ASSERT_EQ(rows.size(), 1u) << "a tag matches as a title does";
    EXPECT_EQ(rows[0].Label, "dock-2") << "an untitled mark-up shows its tag";
    EXPECT_EQ(std::any_cast<MarkupPick>(rows[0].UserData).Token, "@markup:dock-2");

    search.BeginSearch("", collect);
    ASSERT_EQ(rows.size(), 4u);
    EXPECT_EQ(rows[3].Label, "Untitled mark-up") << "one with neither a title nor a tag";
    EXPECT_EQ(rows[3].Detail, "Requested \xC2\xB7 volume");
    EXPECT_EQ(std::any_cast<MarkupPick>(rows[3].UserData).Refusal, "Give this mark-up a title or a tag to name it");

    search.BeginSearch("nothing like it", collect);
    EXPECT_TRUE(rows.empty());
    EXPECT_EQ(search.FormatResultSummary(rows), "No mark-up's title or tag holds nothing like it");
}

// Mark-ups that share a title are told apart: a tagged one is named by its tag; an untagged one
// cannot be named until it has a tag or another title, and only its refusal shows its entity id.
TEST(MarkupContext, TheAtListTellsApartMarkupsThatShareATitle)
{
    MarkupSearchProvider search(
        json::array({Row(7, "Meadow", "", "region"), Row(8, "Meadow", "north", "region"), Row(9, "Pond", "", "volume")}));
    std::vector<SearchResultItem> rows;
    search.BeginSearch("meadow", [&rows](std::vector<SearchResultItem> batch, bool) { rows = std::move(batch); });
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].Detail, "Requested \xC2\xB7 region");
    const MarkupPick untagged = std::any_cast<MarkupPick>(rows[0].UserData);
    EXPECT_TRUE(untagged.Token.empty());
    EXPECT_EQ(untagged.Refusal,
              "2 mark-ups are titled Meadow: give this one (entity 7) a tag or another title to name it");
    EXPECT_EQ(rows[1].Detail, "Requested \xC2\xB7 region \xC2\xB7 tag north");
    EXPECT_EQ(std::any_cast<MarkupPick>(rows[1].UserData).Token, "@markup:north");
}

// A volume's block, whole: its tags, title, status and kind, its shape, its description and its
// thread, each entry its author, time and text.
TEST(MarkupContext, AVolumeBlockCarriesItsShapeDescriptionAndThread)
{
    EXPECT_EQ(MarkupContext::Block(Village()),
              "<markup tags=\"\" title=\"Village\" status=\"Requested\" kind=\"volume\" shape=\"box\" center=\"10,0,20\" "
              "halfExtents=\"5,2,5\" rotation=\"0,0,0,1\">\n"
              "Market square &amp; well\n"
              "Agent 2026-10-13T09:00:00Z created\n"
              "User 2026-10-13T09:00:00Z Keep the well\n"
              "User 2026-10-13T09:00:00Z status Requested\n"
              "</markup>");
}

// Attribute values escape &, <, > and " and turn a line break into a space; body text (the
// description and the thread) escapes &, < and >, so nothing written in them closes the block or
// opens another.
TEST(MarkupContext, TitlesAndBodyTextAreEscapedSoNoTextClosesTheBlock)
{
    json markup = Village();
    markup["title"] = "A \"q\" <b> & x\nnext";
    markup["description"] = "body </markup><markup title=\"evil\"> & <x>";
    markup["entries"] = json::array({Entry("User", "quote </markup> <member mode=\"exclude\"/>")});
    const std::string block = MarkupContext::Block(markup);
    EXPECT_EQ(Attribute(block, "title"), "A &quot;q&quot; &lt;b&gt; &amp; x next");
    EXPECT_NE(block.find("\nbody &lt;/markup&gt;&lt;markup title=\"evil\"&gt; &amp; &lt;x&gt;\n"), std::string::npos)
        << block;
    EXPECT_NE(block.find("quote &lt;/markup&gt; &lt;member mode=\"exclude\"/&gt;"), std::string::npos);
    EXPECT_EQ(block.find("</markup>"), block.size() - 9) << "the block's own end is its only closing tag";
    EXPECT_EQ(block.find("<member"), std::string::npos);
}

// A volume's rotation keeps four decimals (a 30 degree yaw stays a unit quaternion) and its extents
// two (a 4 cm half-extent does not read 0), in the same text whatever the C locale.
TEST(MarkupContext, ARotationKeepsFourDecimalsAndSmallExtentsTwoInAnyLocale)
{
    json markup = Village();
    markup["shape"]["halfExtents"] = {0.04, 2.0, 5.126};
    markup["shape"]["rotation"] = {0.0, 0.258819, 0.0, 0.965926};
    const char* previous = std::setlocale(LC_NUMERIC, nullptr);
    const std::string restore = previous ? previous : "C";
    std::setlocale(LC_NUMERIC, "de-DE");
    const std::string block = MarkupContext::Block(markup);
    std::setlocale(LC_NUMERIC, restore.c_str());
    EXPECT_EQ(Attribute(block, "rotation"), "0,0.2588,0,0.9659");
    EXPECT_EQ(Attribute(block, "halfExtents"), "0.04,2,5.13");
}

// A smooth region whose evaluated ring has 1,000 points: the ring decimated to at most 64 points,
// the area, the bounds and the height, and the whole block under 1 KB.
TEST(MarkupContext, ARegionBlockCarriesItsDecimatedRingAreaBoundsAndHeightUnderOneKilobyte)
{
    json meadow = Meadow(Ring(Circle(100.0, 1000)));
    meadow["shape"]["knots"] = Ring(Circle(100.0, 200));
    meadow["shape"]["type"] = "smooth";
    const std::string block = MarkupContext::Block(meadow);
    EXPECT_LE(PointCount(Attribute(block, "outline")), MarkupContext::kMaxShapePoints);
    EXPECT_GE(PointCount(Attribute(block, "outline")), 16u);
    EXPECT_EQ(Attribute(block, "kind"), "region");
    EXPECT_EQ(Attribute(block, "area"), "31416");
    EXPECT_EQ(Attribute(block, "bounds"), "-100,-100 100,100");
    EXPECT_EQ(Attribute(block, "extrudeHeight"), "8");
    EXPECT_LT(block.size(), 1024u) << block;
}

// A region's area is the area its outline encloses, what the inspector's Outline area shows; its
// net area is that with its members applied (the live region less its excluded pond).
TEST(MarkupContext, ARegionsAreaIsItsOutlinesAndItsNetAreaHasItsMembersApplied)
{
    json meadow = Meadow(Ring({{0, 0}, {30, 0}, {40, 10}, {45, 25}, {40, 40}, {30, 48}, {15, 50}, {0, 45}, {-10, 35},
                               {-12, 20}, {-8, 8}, {-3, 2}}));
    meadow["shape"]["area"] = 2233.6;
    const std::string block = MarkupContext::Block(meadow);
    EXPECT_EQ(Attribute(block, "area"), "2284");
    EXPECT_EQ(Attribute(block, "netArea"), "2234");
}

// A region's members: each its mode, kind, title and tag and its footprint, a circle or an outline;
// a deleted one says so.
TEST(MarkupContext, ARegionBlockListsItsMembers)
{
    json forest = Meadow(Ring({{0, 0}, {200, 0}, {200, 200}, {0, 200}}));
    forest["shape"]["members"] = json::array(
        {json{{"entityId", 11}, {"mode", "exclude"}, {"deleted", false}, {"title", "Clearing"}, {"tag", "scene-11"},
              {"tags", json::array()}, {"kind", "region"}, {"footprint", {{"outline", Ring({{80, 80}, {120, 80}, {120, 120}, {80, 120}})}}}},
         json{{"entityId", 12}, {"mode", "exclude"}, {"deleted", false}, {"title", "Pond"}, {"tag", "scene-12"},
              {"tags", json::array({"pond", "old \"town\""})}, {"kind", "volume"}, {"footprint", {{"center", {50.0, 50.0}}, {"radius", 12.5}}}},
         json{{"entityId", 13}, {"mode", "include"}, {"deleted", true}}});
    const std::string block = MarkupContext::Block(forest);
    EXPECT_NE(block.find("<member mode=\"exclude\" kind=\"region\" title=\"Clearing\" tags=\"\" "
                         "outline=\"80,80 120,80 120,120 80,120\"/>"),
              std::string::npos)
        << block;
    // Each tag quoted, a quote in a tag as \", so a tag with a space stays one tag; never the scene tag.
    EXPECT_NE(block.find("<member mode=\"exclude\" kind=\"volume\" title=\"Pond\" "
                         "tags=\"&quot;pond&quot; &quot;old \\&quot;town\\&quot;&quot;\" center=\"50,50\" "
                         "radius=\"12.5\"/>"),
              std::string::npos)
        << block;
    EXPECT_NE(block.find("<member mode=\"include\" deleted=\"true\"/>"), std::string::npos);
}

// A region the evaluator cannot outline (two knots, or edges that cross) carries its knots and the
// outline's problem with its fix, and no area.
TEST(MarkupContext, ARegionThatCannotBeOutlinedCarriesItsKnotsAndTheProblem)
{
    json two = Meadow(json::array());
    two["shape"]["knots"] = Ring({{0, 0}, {30, 0}});
    const std::string twoBlock = MarkupContext::Block(two);
    EXPECT_EQ(Attribute(twoBlock, "knots"), "0,0 30,0");
    EXPECT_NE(Attribute(twoBlock, "problem").find("An outline needs 3 to 256 points"), std::string::npos);
    EXPECT_EQ(Attribute(twoBlock, "area"), "");

    const json bowtie = Ring({{0, 0}, {40, 40}, {40, 0}, {0, 40}});
    const std::string crossed = MarkupContext::Block(Meadow(bowtie));
    EXPECT_EQ(Attribute(crossed, "knots"), "0,0 40,40 40,0 0,40");
    EXPECT_NE(Attribute(crossed, "problem").find("crosses itself"), std::string::npos);
    EXPECT_EQ(Attribute(crossed, "outline"), "");
}

// A path's block carries its points (at most 64) and its length.
TEST(MarkupContext, APathBlockCarriesItsPointsAndLength)
{
    json path = Row(5, "Road", "", "path");
    json points = json::array();
    for (int i = 0; i < 200; ++i)
        points.push_back({i * 1.0, 0.0, std::sin(i * 0.1) * 20.0});
    path["shape"] = json{{"shape", "path"}, {"points", points}, {"type", "linear"}, {"length", 412.5}};
    const std::string block = MarkupContext::Block(path);
    EXPECT_LE(PointCount(Attribute(block, "points")), MarkupContext::kMaxShapePoints);
    EXPECT_EQ(Attribute(block, "length"), "412.5");
    EXPECT_EQ(Attribute(block, "points").rfind("0,0,0 ", 0), 0u);
}

// A block stops at 8 KB: the thread's newest ten entries are shown while they fit after the
// description, each cut at 500 bytes, and those that did not fit are counted.
TEST(MarkupContext, ABlockStopsAtEightKilobytesAndCountsWhatItLeftOut)
{
    json markup = Village();
    markup["description"] = std::string(7000, 'd');
    markup["entries"] = json::array();
    for (int i = 0; i < 30; ++i)
        markup["entries"].push_back(Entry("User", std::string(900, 'e')));
    const std::string block = MarkupContext::Block(markup);
    EXPECT_LE(block.size(), MarkupContext::kMaxBlockBytes);
    EXPECT_NE(block.find("[not shown, the 8 KB block cap: 9 entries]"), std::string::npos) << block;
    const std::size_t entryLine = block.find("\nUser ");
    ASSERT_NE(entryLine, std::string::npos);
    EXPECT_EQ(block.find('\n', entryLine + 1) - entryLine - 1, MarkupContext::kMaxEntryBytes);
}

// A cut never splits an escaped character: the description and an entry are cut on whole
// characters, the entry at 500 of the user's bytes, and what was left out is counted in the
// user's bytes.
TEST(MarkupContext, ACutNeverSplitsAnEscapedCharacter)
{
    json markup = Village();
    markup["description"] = std::string(8000, '&');
    markup["entries"] = json::array({Entry("User", std::string(600, '<'))});
    const std::string block = MarkupContext::Block(markup);
    const std::size_t start = block.find(">\n") + 2;
    const std::string description = block.substr(start, block.find('\n', start) - start);
    ASSERT_FALSE(description.empty());
    EXPECT_EQ(description.size() % 5, 0u) << "whole &amp; entities only";
    EXPECT_EQ(description.find_first_not_of("&amp;"), std::string::npos);
    const std::size_t kept = description.size() / 5;
    EXPECT_NE(block.find("the description's last " + std::to_string(8000 - kept) + " bytes"), std::string::npos)
        << block.substr(block.size() - 200);

    json thread = Village();
    thread["entries"] = json::array({Entry("User", std::string(600, '<'))});
    const std::string entryBlock = MarkupContext::Block(thread);
    const std::size_t line = entryBlock.find("\nUser ") + 1;
    const std::string entry = entryBlock.substr(line, entryBlock.find('\n', line) - line);
    const std::string head = "User 2026-10-13T09:00:00Z ";
    EXPECT_EQ(entry.rfind(head, 0), 0u);
    EXPECT_EQ(entry.size() - head.size(), (MarkupContext::kMaxEntryBytes - head.size()) * 4)
        << "500 of the user's bytes, each < written as &lt;";
}

// A description too long for the block is cut, and the region's members stay: they are its shape.
TEST(MarkupContext, ALongDescriptionIsCutBeforeTheMembers)
{
    json forest = Meadow(Ring({{0, 0}, {200, 0}, {200, 200}, {0, 200}}));
    forest["description"] = std::string(9000, 'd');
    forest["shape"]["members"] = json::array(
        {json{{"entityId", 12}, {"mode", "exclude"}, {"deleted", false}, {"title", "Pond"}, {"tag", ""},
              {"kind", "volume"}, {"footprint", {{"center", {50.0, 50.0}}, {"radius", 12.5}}}}});
    const std::string block = MarkupContext::Block(forest);
    EXPECT_LE(block.size(), MarkupContext::kMaxBlockBytes);
    EXPECT_NE(block.find("<member mode=\"exclude\" kind=\"volume\" title=\"Pond\""), std::string::npos);
    EXPECT_NE(block.find("[not shown, the 8 KB block cap: the description's last "), std::string::npos);
}

// A prompt's context: a block per named mark-up read through the seam, the unknown one named and
// said on the status line, every token refused in play mode, and the turn stopped at 32 KB.
TEST(MarkupContext, APromptsContextReadsEachNamedMarkupThroughTheEditor)
{
    UninstallGuard guard;
    FakeScene scene;
    scene.Rows = json::array({Row(7, "Village", "", "volume"), Row(9, "Meadow", "meadow-1", "region")});
    scene.Markups[7] = Village();
    scene.Markups[9] = Meadow(Ring({{0, 0}, {50, 0}, {50, 50}}));
    scene.Install();
    const Editor::EditorMarkupRequests& requests = Editor::EditorMarkupRequests::Get();

    EXPECT_TRUE(MarkupContext::ForPrompt("No tokens here", requests).Text.empty());
    const MarkupTurnContext both =
        MarkupContext::ForPrompt("Join @markup:meadow-1 to @markup:\"Village\" and @markup:nowhere", requests);
    EXPECT_EQ(both.Text.find("<markup tags=\"&quot;meadow-1&quot;\""), 0u);
    EXPECT_NE(both.Text.find("title=\"Village\""), std::string::npos);
    EXPECT_NE(both.Text.find("\n[unknown mark-up @markup:nowhere]"), std::string::npos);
    EXPECT_EQ(both.Notice, "No mark-up is named @markup:nowhere; the agent is told so");
    // Each token keeps the title of the mark-up it names now, for its chip.
    ASSERT_EQ(both.Mentions.size(), 3u);
    EXPECT_EQ(both.Mentions[0].Token, "@markup:meadow-1");
    EXPECT_EQ(both.Mentions[0].Title, "Meadow");
    EXPECT_EQ(both.Mentions[1].Title, "Village");
    EXPECT_EQ(both.Mentions[2].Title, "");
    EXPECT_EQ(MarkupContext::ForPrompt("Look at @markup:scene-9", requests).Text, "[unknown mark-up @markup:scene-9]")
        << "a scene tag names no mark-up";

    scene.Markups[7]["description"] = std::string(7800, 'd');
    std::string many;
    for (int i = 0; i < 6; ++i)
    {
        scene.Rows.push_back(Row(100 + i, "Big " + std::to_string(i), "", "volume"));
        scene.Markups[100 + i] = scene.Markups[7];
        many += "@markup:\"Big " + std::to_string(i) + "\" ";
    }
    const MarkupTurnContext capped = MarkupContext::ForPrompt(many, requests);
    EXPECT_LE(capped.Text.size(), MarkupContext::kMaxTurnBytes);
    EXPECT_NE(capped.Text.find("more mark-up"), std::string::npos);
    EXPECT_NE(capped.Text.find(" not included: the 32 KB per-turn cap]"), std::string::npos);

    // Unknown tokens count against the cap too: 3,000 of them stay within 32 KB, the rest counted.
    std::string unknown;
    for (int i = 0; i < 3000; ++i)
        unknown += "@markup:nowhere-" + std::to_string(i) + " ";
    const MarkupTurnContext unknowns = MarkupContext::ForPrompt(unknown, requests);
    EXPECT_LE(unknowns.Text.size(), MarkupContext::kMaxTurnBytes);
    EXPECT_NE(unknowns.Text.find(" more mark-ups not included: the 32 KB per-turn cap]"), std::string::npos);

    scene.ListRefusal = "Exit play mode first: mark-ups are read and edited in the edit world";
    const MarkupTurnContext playing = MarkupContext::ForPrompt("Look at @markup:meadow-1", requests);
    EXPECT_EQ(playing.Text, "[mark-up @markup:meadow-1 not read: " + scene.ListRefusal + "]");
    EXPECT_EQ(playing.Notice, scene.ListRefusal);
}

// What a turn sends is the prompt then its context, for the turn and in a resent history; the
// row shows the prompt alone, with a chip per named mark-up titled as it was when it was sent.
TEST(MarkupContext, ATurnSendsItsContextAfterThePromptAndTheRowShowsChips)
{
    Conversation conversation;
    const Conversation::TurnId turn = conversation.AddTurn(
        "Widen @markup:road-2 by @markup:\"Village\"",
        SentMarkups{"<markup …>", {{"@markup:road-2", "North road"}, {"@markup:\"Village\"", ""}}}, "claude-api", "",
        "", {});
    const ConversationMessage& user = *conversation.UserMessage(turn);
    EXPECT_EQ(user.SentText(), "Widen @markup:road-2 by @markup:\"Village\"\n\n<markup …>");

    AgentTurnRequest request;
    request.History = conversation.Messages().subspan(0, 1);
    request.UserText = "next";
    const json body = json::parse(ClaudeApiProvider::BuildRequestBody(request));
    EXPECT_EQ(body["messages"][0]["content"], user.SentText());

    const AgentConversationRowModel row = AgentConversationRowModel::From(user, {}, turn, AssistantUndoSnapshot{});
    EXPECT_EQ(row.Text, user.Text);
    EXPECT_EQ(row.MarkupChips,
              (std::vector<MarkupChip>{{"@markup:road-2", "North road"}, {"@markup:\"Village\"", "Village"}}));
}

// "Ask the AI Assistant" puts the mark-up's token in the prompt once: its tag (the first of the
// tags its Tags row edits, never its scene tag) when it has one, else its title. A mark-up no token
// names apart, an untitled untagged one or an untagged one whose title another has, is refused
// with the `@` list's own words, and nothing is put in the prompt.
TEST(MarkupContext, AskingAboutAMarkupPrefillsItsTokenOnce)
{
    using MarkupECS::MarkupService;
    MarkupService::Initialize();
    struct ServiceGuard
    {
        ~ServiceGuard() { MarkupService::Shutdown(); }
    } serviceGuard;
    ECS::World world;
    const ECS::EntityHandle village = CreateMarkup(world, "Village center");
    AskAboutMarkup(world, village);
    EXPECT_EQ(AiAssistantPanelService::TakePrefill(), "@markup:\"Village center\" ");
    EXPECT_EQ(AiAssistantPanelService::TakePrefill(), "");

    const float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    const uint32_t tags[] = {MarkupService::Get().AddTag("v-1", "", color), MarkupService::Get().AddTag("old", "", color)};
    ASSERT_TRUE(MarkupService::Get().SetTags(world, village, tags, Components::MarkupAuthor::User, 0));
    AskAboutMarkup(world, village);
    EXPECT_EQ(AiAssistantPanelService::TakePrefill(), "@markup:v-1 ");
    EXPECT_EQ(AskDisabledReason(world, village), "");

    const ECS::EntityHandle twin = CreateMarkup(world, "Village center");
    EXPECT_EQ(AskDisabledReason(world, village), "") << "its tag still names it apart";
    EXPECT_EQ(AskDisabledReason(world, twin), "2 mark-ups are titled Village center: give this one (entity " +
                                                  std::to_string(twin.id) + ") a tag or another title to name it");
    AskAboutMarkup(world, twin);
    EXPECT_EQ(AiAssistantPanelService::TakePrefill(), "") << "never the other mark-up's token";

    const ECS::EntityHandle unnamed = CreateMarkup(world, "");
    EXPECT_EQ(AskDisabledReason(world, unnamed), "Give this mark-up a title or a tag to ask the AI Assistant about it");
    AskAboutMarkup(world, unnamed);
    EXPECT_EQ(AiAssistantPanelService::TakePrefill(), "") << "no token, nothing put in the prompt";
}

// "Comment on mark-up" shows on a reply to a prompt that names mark-ups and adds the reply, cut at
// 2,000 characters and introduced as the assistant's, to each named one's thread as the user's.
TEST(MarkupContext, CommentingAReplyAddsItToEachNamedMarkupAsTheUsers)
{
    UninstallGuard guard;
    FakeScene scene;
    scene.Rows = json::array({Row(7, "Village", "", "volume"), Row(9, "Meadow", "meadow-1", "region")});
    scene.Install();
    RegisterMarkupActions();
    const AgentReplyAction* comment = nullptr;
    for (const AgentReplyAction& action : AgentReplyActions::All())
    {
        if (action.Id == "ai-assistant.comment-on-markup")
            comment = &action;
    }
    ASSERT_NE(comment, nullptr);

    ConversationMessage prompt;
    prompt.Text = "Compare @markup:\"Village\" with @markup:meadow-1";
    ConversationMessage plain;
    plain.Text = "Hello";
    EXPECT_TRUE(comment->Once) << "a second click cannot add the reply again";
    EXPECT_TRUE(comment->ShownFor(prompt));
    EXPECT_FALSE(comment->ShownFor(plain));

    ConversationMessage reply;
    reply.Role = MessageRole::Assistant;
    reply.ProviderId = "claude-session";
    reply.AnsweredModel = "claude-opus-5-5";
    std::string accents;
    for (int i = 0; i < 2500; ++i)
        accents += "\xC3\xA9";
    reply.Text = accents;
    const AgentReplyOutcome added = comment->Run(reply, prompt);
    EXPECT_EQ(added.Status, "Reply added to the thread of Village, Meadow");
    EXPECT_TRUE(added.Acted);
    ASSERT_EQ(scene.Comments.size(), 2u);
    EXPECT_EQ(scene.Comments[0]["entityId"], 7);
    EXPECT_EQ(scene.Comments[1]["entityId"], 9);
    EXPECT_EQ(scene.Comments[0]["author"], "User");
    const std::string text = scene.Comments[0]["text"];
    const std::string prefix = "From the assistant (" + AgentConversationRowModel::ReplyHeader(reply) + "): ";
    EXPECT_EQ(prefix.find("claude-session"), std::string::npos) << "the connection's name, not its id";
    EXPECT_EQ(text.rfind(prefix, 0), 0u);
    // 2,000 characters of two bytes each, then the three-byte ellipsis.
    EXPECT_EQ(text.size(), prefix.size() + 2000 * 2 + 3);

    // In play mode the comment is refused: the status line says why, and it did not act, so the
    // button stays on for a try after play.
    scene.ListRefusal = "Exit play mode first: mark-ups are read and edited in the edit world";
    const AgentReplyOutcome refused = comment->Run(reply, prompt);
    EXPECT_EQ(refused.Status, scene.ListRefusal);
    EXPECT_FALSE(refused.Acted);
    EXPECT_EQ(scene.Comments.size(), 2u);
    const std::vector<const Editor::MarkupAction*> markupActions = Editor::MarkupActionRegistry::Get().GetActions();
    EXPECT_TRUE(std::any_of(markupActions.begin(), markupActions.end(), [](const Editor::MarkupAction* action)
                            { return action->Id == "ai-assistant.ask" && action->Label == "Ask the AI Assistant"; }));
}
