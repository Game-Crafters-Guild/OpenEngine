// The mark-up seam package modules reach mark-ups through (EditorMarkupRequests, installed by
// the editor) and the actions they offer on a mark-up (MarkupActionRegistry). Links
// EditorSDK.dll, the same instances Editor.exe and module DLLs share; process-wide, so the
// handlers are uninstalled before each test ends.

#include "ECS/ModuleRegistration.h"
#include "Editor/Registries/EditorMarkupRequests.h"
#include "Editor/Registries/MarkupActionRegistry.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace ed = GameEngine::Editor;
namespace ecs = GameEngine::ECS;

namespace
{
struct UninstallGuard
{
    ~UninstallGuard() { ed::EditorMarkupRequests::Get().Install({}); }
};

const ed::MarkupAction* FindAction(std::string_view id)
{
    for (const ed::MarkupAction* action : ed::MarkupActionRegistry::Get().GetActions())
    {
        if (action->Id == id)
            return action;
    }
    return nullptr;
}
} // namespace

// A host that installed nothing (not the editor) refuses every request and says why.
TEST(EditorMarkupRequests, EveryRequestRefusesWhileNoneIsInstalled)
{
    UninstallGuard guard;
    const ed::EditorMarkupRequests& requests = ed::EditorMarkupRequests::Get();
    for (const ed::MarkupRequestAnswer& answer :
         {requests.List(nlohmann::json::object()), requests.Query(ecs::EntityHandle(7)),
          requests.Comment(ecs::EntityHandle(7), "Hi", GameEngine::Components::MarkupAuthor::User),
          requests.Frame(ecs::EntityHandle(7))})
    {
        EXPECT_TRUE(answer.IsRefusal());
        EXPECT_NE(answer.Refusal.find("no mark-up requests installed"), std::string::npos) << answer.Refusal;
    }
}

// Each request hands the installed handler the markup_* method's parameters and returns its
// answer unchanged, a refusal included.
TEST(EditorMarkupRequests, EachRequestReachesItsHandlerWithTheMethodsParameters)
{
    UninstallGuard guard;
    std::vector<nlohmann::json> seen;
    const auto record = [&seen](const char* name) {
        return [&seen, name](const nlohmann::json& params) {
            seen.push_back(nlohmann::json{{"handler", name}, {"params", params}});
            return ed::MarkupRequestAnswer{nlohmann::json{{"from", name}}, {}};
        };
    };
    ed::EditorMarkupRequests::Handlers handlers;
    handlers.List = record("list");
    handlers.Query = record("query");
    handlers.Comment = record("comment");
    handlers.Frame = [](const nlohmann::json&) { return ed::MarkupRequestAnswer{{}, "No Scene View"}; };
    ed::EditorMarkupRequests::Get().Install(std::move(handlers));

    const ed::EditorMarkupRequests& requests = ed::EditorMarkupRequests::Get();
    EXPECT_EQ(requests.List(nlohmann::json{{"status", "Requested"}}).Result["from"], "list");
    const ecs::EntityHandle road(12);
    EXPECT_EQ(requests.Query(road).Result["from"], "query");
    EXPECT_EQ(requests.Comment(road, "Widen the road", GameEngine::Components::MarkupAuthor::Agent).Result["from"],
              "comment");
    const ed::MarkupRequestAnswer frame = requests.Frame(road);
    EXPECT_TRUE(frame.IsRefusal());
    EXPECT_EQ(frame.Refusal, "No Scene View");

    ASSERT_EQ(seen.size(), 3u);
    EXPECT_EQ(seen[0]["params"], (nlohmann::json{{"status", "Requested"}}));
    EXPECT_EQ(seen[1]["params"], (nlohmann::json{{"entityId", 12}}));
    EXPECT_EQ(seen[2]["params"], (nlohmann::json{{"entityId", 12}, {"text", "Widen the road"}, {"author", "Agent"}}));
}

// An action registered again under its id replaces the first in place, stamped with the module
// that registered it; an action without an id, a label or a callable, or whose label holds a '/'
// (a submenu path in the panel's context menu), is refused.
TEST(MarkupActionRegistry, AnIdRegisteredAgainReplacesItInPlaceAndPinsItsModule)
{
    ed::MarkupActionRegistry& registry = ed::MarkupActionRegistry::Get();
    const auto noop = [](const ecs::World&, ecs::EntityHandle) {};
    const std::size_t before = registry.GetActions().size();
    {
        ecs::SetActiveRegistrationModule("MarkupActionPack", 1);
        registry.Register({"test.first", "First", "", noop});
        registry.Register({"test.second", "Second", "", noop});
        registry.Register({"test.first", "First again", "", noop});
        registry.Register({"", "No id", "", noop});
        registry.Register({"test.no-action", "No action", "", {}});
        registry.Register({"test.slash", "Ask/Tell", "", noop});
        ecs::ClearActiveRegistrationModule();
    }

    const std::vector<const ed::MarkupAction*> actions = registry.GetActions();
    ASSERT_EQ(actions.size(), before + 2);
    EXPECT_EQ(actions[before]->Id, "test.first");
    EXPECT_EQ(actions[before]->Label, "First again");
    EXPECT_EQ(actions[before + 1]->Id, "test.second");
    EXPECT_EQ(FindAction("test.no-action"), nullptr);
    EXPECT_EQ(FindAction("test.slash"), nullptr);

    std::vector<std::string> pins;
    registry.AppendModulePins("MarkupActionPack", pins);
    EXPECT_EQ(pins, (std::vector<std::string>{"mark-up action 'test.first'", "mark-up action 'test.second'"}));
    std::vector<std::string> otherPins;
    registry.AppendModulePins("OtherPack", otherPins);
    EXPECT_TRUE(otherPins.empty());
}
