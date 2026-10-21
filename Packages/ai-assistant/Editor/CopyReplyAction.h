#pragma once

#include "AgentReplyActions.h"

namespace GameEngine
{
struct ConversationMessage;

/// The Copy reply action: puts the reply's stored text on the clipboard; CopyReplyFeedback(reply)
/// is the panel's status line.
AgentReplyOutcome CopyReply(const ConversationMessage& reply, const ConversationMessage& prompt);
} // namespace GameEngine
