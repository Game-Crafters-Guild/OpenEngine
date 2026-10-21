#pragma once

#include <functional>
#include <span>
#include <string>

namespace GameEngine
{
struct ConversationMessage;

/// What a reply action did when clicked.
struct AgentReplyOutcome
{
    /// What the panel's status line reads after it ("Reply copied"); empty leaves the line as is.
    std::string Status;
    /// Whether it did what it offers; false when it was refused (a mark-up comment in play mode),
    /// which leaves a Once action's button on for another try.
    bool Acted = true;
};

/// A button on every finished assistant reply in the AI Assistant panel.
struct AgentReplyAction
{
    /// Stable identifier; the button's element id is built from it.
    std::string Id;
    /// The button text.
    std::string Label;
    /// The button tooltip.
    std::string Tooltip;
    /// Runs on the UI thread with the reply the button sits on and the prompt it answers.
    std::function<AgentReplyOutcome(const ConversationMessage& reply, const ConversationMessage& prompt)> Run;
    /// Whether a reply to `prompt` offers the action; null offers it on every reply.
    std::function<bool(const ConversationMessage& prompt)> ShownFor;
    /// The action runs once per reply: after a click that acted (AgentReplyOutcome::Acted) the
    /// button is disabled, its tooltip DoneTooltip, so a second click cannot repeat what the
    /// first did.
    bool Once = false;
    std::string DoneTooltip;
};

/// The per-reply actions, registered by the module that owns each one (the package
/// registers Copy), shown in registration order. Register at module load, from the
/// UI thread; rows read the list when they are built.
class AgentReplyActions
{
public:
    static void Register(AgentReplyAction action);
    static std::span<const AgentReplyAction> All();
};
} // namespace GameEngine
