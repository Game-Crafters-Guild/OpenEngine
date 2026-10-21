#include "MarkupChips.h"

#include "MarkupContext.h"

#include "Editor/Registries/EditorMarkupRequests.h"

#include <nlohmann/json.hpp>

namespace GameEngine
{
std::string FrameMarkupChip(const std::string& token)
{
    const Editor::EditorMarkupRequests& requests = Editor::EditorMarkupRequests::Get();
    const std::vector<MarkupToken> tokens = MarkupContext::FindTokens(token);
    const Editor::MarkupRequestAnswer list = requests.List(nlohmann::json::object());
    if (list.IsRefusal())
        return list.Refusal;
    const nlohmann::json* row =
        tokens.empty() ? nullptr : MarkupContext::Resolve(tokens.front(), list.Result["markups"]);
    if (!row)
        return "No mark-up is named " + token + " now";
    const Editor::MarkupRequestAnswer framed = requests.Frame(MarkupContext::EntityOf(*row));
    return framed.IsRefusal() ? framed.Refusal : "Framed " + row->value("title", token);
}
} // namespace GameEngine
