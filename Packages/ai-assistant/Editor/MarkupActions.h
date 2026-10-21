#pragma once

#include "AgentReplyActions.h"

#include "ECS/ECS.h"

#include <string>

namespace GameEngine
{
namespace ECS
{
class World;
}
struct ConversationMessage;

/// "Ask the AI Assistant" on a mark-up (Editor::MarkupActionRegistry): opens the AI Assistant
/// panel with the mark-up's token in its prompt (AiAssistantPanelService::Prefill). Does nothing
/// for a mark-up no token can name (AskDisabledReason).
void AskAboutMarkup(const ECS::World& world, ECS::EntityHandle markup);
/// Why Ask the AI Assistant is off for `markup`, with the fix: "Give this mark-up a title or a tag
/// to ask the AI Assistant about it" when it has neither, else the `@` list's refusal when no token
/// names it apart (MarkupContext::NameOf). Empty when the action can run.
std::string AskDisabledReason(const ECS::World& world, ECS::EntityHandle markup);

/// "Comment on mark-up" on a reply whose prompt names mark-ups: adds the reply, cut to 2,000
/// characters and introduced as the assistant's, to each named mark-up's thread as the user's
/// comment. The status line names the mark-ups it went to, or why not; it acted when the reply
/// went to at least one.
AgentReplyOutcome CommentReplyOnMarkups(const ConversationMessage& reply, const ConversationMessage& prompt);

/// The reply's text as the comment carries it: "From the assistant (<the reply's header>): "
/// (AgentConversationRowModel::ReplyHeader) then the reply, cut after 2,000 characters (code
/// points) with an ellipsis.
std::string MarkupCommentText(const ConversationMessage& reply);

/// Registers the two actions with the editor at module load.
void RegisterMarkupActions();
} // namespace GameEngine
