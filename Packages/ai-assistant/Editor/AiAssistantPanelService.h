#pragma once

#include <string>

namespace GameEngine
{
/// Hands text to the AI Assistant panel's prompt from outside the panel ("Ask the AI
/// Assistant" on a mark-up). Main thread only.
class AiAssistantPanelService
{
public:
    /// Text the open panel adds to its prompt at its next update, the field focused; a second
    /// call before then replaces it.
    static void Prefill(std::string text);
    /// The text waiting for the prompt, once: empty when none waits.
    static std::string TakePrefill();
};
} // namespace GameEngine
