#include "AiAssistantPanelService.h"

#include <utility>

namespace GameEngine
{
namespace
{
std::string& Pending()
{
    static std::string pending;
    return pending;
}
} // namespace

void AiAssistantPanelService::Prefill(std::string text)
{
    Pending() = std::move(text);
}

std::string AiAssistantPanelService::TakePrefill()
{
    return std::exchange(Pending(), std::string());
}
} // namespace GameEngine
