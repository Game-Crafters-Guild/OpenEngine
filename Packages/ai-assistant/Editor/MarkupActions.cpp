#include "MarkupActions.h"

#include "AgentConversationRowModel.h"
#include "AgentReplyActions.h"
#include "AiAssistantPanelService.h"
#include "ConversationMessage.h"
#include "MarkupContext.h"

#include "Components/Markup/Markup.h"
#include "Components/Name.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Editor/Registries/EditorMarkupRequests.h"
#include "Editor/Registries/EditorPanelRegistry.h"
#include "Editor/Registries/MarkupActionRegistry.h"
#include "MarkupECS/MarkupService.h"
#include "UI/EditorIcons.h"

#include <nlohmann/json.hpp>

#include <cstddef>

namespace GameEngine
{
namespace
{
// The panel the actions open (AiAssistantEditorModuleInit registers it).
constexpr const char* kPanelId = "AIAssistant";
// The most of a reply a comment carries, in characters.
constexpr std::size_t kMaxCommentChars = 2000;

// `text` cut after `chars` characters (UTF-8 code points), with an ellipsis when cut.
std::string CutCharacters(const std::string& text, std::size_t chars)
{
    std::size_t counted = 0;
    for (std::size_t at = 0; at < text.size(); ++at)
    {
        if ((static_cast<unsigned char>(text[at]) & 0xC0) == 0x80)
            continue;
        if (counted++ == chars)
            return text.substr(0, at) + "\xE2\x80\xA6";
    }
    return text;
}

std::string_view TitleOf(const ECS::World& world, ECS::EntityHandle markup)
{
    const auto* name = world.GetComponent<Components::Name>(markup);
    return name ? name->View() : std::string_view();
}

// The first of the mark-up's tags, as markup_list's `tags` lists them; empty when it has none.
std::string_view FirstTagOf(const MarkupECS::MarkupService& service, const ECS::World& world,
                            ECS::EntityHandle markup)
{
    const MarkupECS::MarkupNotes* notes = service.FindNotes(world, markup);
    if (!notes)
        return {};
    for (const uint32 id : notes->Tags)
    {
        const MarkupECS::MarkupTag* tag = id == MarkupECS::MarkupService::kInvalidTag ? nullptr : service.GetTag(id);
        if (tag)
            return tag->Name;
    }
    return {};
}

// What names `markup` (MarkupContext::NameOf), read from the components and the notes rather than
// through markup_list: the inspector asks for it every frame it shows Ask. The views live while the
// world's names and the tag vocabulary are unchanged.
MarkupNameFacts NameFactsOf(const ECS::World& world, ECS::EntityHandle markup)
{
    const MarkupECS::MarkupService* service = MarkupECS::MarkupService::TryGet();
    MarkupNameFacts facts{.EntityId = markup.id, .Title = TitleOf(world, markup)};
    if (!service)
        return facts;
    facts.Tag = FirstTagOf(*service, world, markup);
    for (const ECS::EntityHandle other : service->GetMarkups(world))
    {
        if (other == markup)
            continue;
        if (!facts.Title.empty() && TitleOf(world, other) == facts.Title)
            ++facts.SameTitle;
        if (!facts.Tag.empty() && FirstTagOf(*service, world, other) == facts.Tag)
            ++facts.SameTag;
    }
    return facts;
}

bool NamesMarkups(const ConversationMessage& prompt)
{
    return !MarkupContext::FindTokens(prompt.Text).empty();
}
} // namespace

void AskAboutMarkup(const ECS::World& world, ECS::EntityHandle markup)
{
    const MarkupPick name = MarkupContext::NameOf(NameFactsOf(world, markup));
    if (name.Token.empty())
        return;
    AiAssistantPanelService::Prefill(name.Token + " ");
    Editor::EditorPanelRegistry::Get().OpenPanel(kPanelId);
}

std::string AskDisabledReason(const ECS::World& world, ECS::EntityHandle markup)
{
    const MarkupNameFacts facts = NameFactsOf(world, markup);
    if (facts.Title.empty() && facts.Tag.empty())
        return "Give this mark-up a title or a tag to ask the AI Assistant about it";
    return MarkupContext::NameOf(facts).Refusal;
}

std::string MarkupCommentText(const ConversationMessage& reply)
{
    return "From the assistant (" + AgentConversationRowModel::ReplyHeader(reply) +
           "): " + CutCharacters(reply.Text, kMaxCommentChars);
}

AgentReplyOutcome CommentReplyOnMarkups(const ConversationMessage& reply, const ConversationMessage& prompt)
{
    const Editor::EditorMarkupRequests& requests = Editor::EditorMarkupRequests::Get();
    const Editor::MarkupRequestAnswer list = requests.List(nlohmann::json::object());
    if (list.IsRefusal())
        return {list.Refusal, false};
    const std::string text = MarkupCommentText(reply);
    std::string commented;
    std::string refusal;
    for (const MarkupToken& token : MarkupContext::FindTokens(prompt.Text))
    {
        const nlohmann::json* row = MarkupContext::Resolve(token, list.Result["markups"]);
        if (!row)
        {
            refusal = "No mark-up is named " + token.Text + " now";
            continue;
        }
        const Editor::MarkupRequestAnswer answer =
            requests.Comment(MarkupContext::EntityOf(*row), text, Components::MarkupAuthor::User);
        if (answer.IsRefusal())
            refusal = answer.Refusal;
        else
            commented += (commented.empty() ? "" : ", ") + row->value("title", token.Text);
    }
    if (commented.empty())
        return {refusal, false};
    return {"Reply added to the thread of " + commented + (refusal.empty() ? "" : "; " + refusal)};
}

void RegisterMarkupActions()
{
    Editor::MarkupActionRegistry::Get().Register(
        {"ai-assistant.ask", "Ask the AI Assistant", EditorIcons::kSparkles, &AskAboutMarkup, &AskDisabledReason});
    AgentReplyActions::Register({"ai-assistant.comment-on-markup", "Comment on mark-up",
                                 "Add this reply to the thread of each mark-up the prompt names, as your comment",
                                 &CommentReplyOnMarkups, &NamesMarkups, true,
                                 "Added to the threads of the mark-ups the prompt names"});
}
} // namespace GameEngine
