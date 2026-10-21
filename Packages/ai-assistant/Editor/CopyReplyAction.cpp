#include "CopyReplyAction.h"

#include "AgentStatusMessages.h"
#include "ConversationMessage.h"

#include "Platform/Clipboard.h"

namespace GameEngine
{
AgentReplyOutcome CopyReply(const ConversationMessage& reply, const ConversationMessage&)
{
    Platform::SetClipboardText(reply.Text.c_str());
    return {CopyReplyFeedback(reply)};
}
} // namespace GameEngine
