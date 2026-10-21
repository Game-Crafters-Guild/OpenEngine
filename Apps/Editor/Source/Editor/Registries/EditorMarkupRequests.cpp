#include "Editor/Registries/EditorMarkupRequests.h"

#include <utility>

namespace GameEngine::Editor
{

namespace
{

// The author as markup_comment reads it.
const char* AuthorName(Components::MarkupAuthor author)
{
    switch (author)
    {
    case Components::MarkupAuthor::User:
        return "User";
    case Components::MarkupAuthor::Agent:
        return "Agent";
    }
    return "User";
}

} // namespace

EditorMarkupRequests& EditorMarkupRequests::Get()
{
    static EditorMarkupRequests s_Instance;
    return s_Instance;
}

void EditorMarkupRequests::Install(Handlers handlers)
{
    m_Handlers = std::move(handlers);
}

MarkupRequestAnswer EditorMarkupRequests::Call(const Request& request, const nlohmann::json& params)
{
    if (!request)
        return MarkupRequestAnswer{{}, "Mark-ups are read in the editor: this host has no mark-up requests installed"};
    return request(params);
}

MarkupRequestAnswer EditorMarkupRequests::List(const nlohmann::json& params) const
{
    return Call(m_Handlers.List, params);
}

MarkupRequestAnswer EditorMarkupRequests::Query(ECS::EntityHandle markup) const
{
    return Call(m_Handlers.Query, nlohmann::json{{"entityId", markup.id}});
}

MarkupRequestAnswer EditorMarkupRequests::Comment(ECS::EntityHandle markup, const std::string& text,
                                                  Components::MarkupAuthor author) const
{
    return Call(m_Handlers.Comment, nlohmann::json{{"entityId", markup.id}, {"text", text}, {"author", AuthorName(author)}});
}

MarkupRequestAnswer EditorMarkupRequests::Frame(ECS::EntityHandle markup) const
{
    return Call(m_Handlers.Frame, nlohmann::json{{"entityId", markup.id}});
}

} // namespace GameEngine::Editor
