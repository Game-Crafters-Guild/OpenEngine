#pragma once

#include "Components/Markup/Markup.h"
#include "ECS/ECS.h"

#include <functional>
#include <string>

#include <nlohmann/json.hpp>

namespace GameEngine::Editor
{

// One answer of the editor's mark-up requests: the result the markup_* debug-server method
// of the same name returns (its JSON, documented in docs/editor/world-markups.mdx), or the
// refusal it gives, whose reason names the fix.
struct MarkupRequestAnswer
{
    nlohmann::json Result;
    std::string Refusal; // empty for an answer that carries a result

    bool IsRefusal() const { return !Refusal.empty(); }
};

// The editor's mark-up requests for package modules, which cannot link the editor's mark-up
// sources: the editor installs the handlers at startup (Markups/MarkupEditorSurfaces), a
// module calls them. Main thread only, as the mark-up methods are. Without installed
// handlers (a host that is not the editor) every call is a refusal that says so.
class EditorMarkupRequests
{
public:
    using Request = std::function<MarkupRequestAnswer(const nlohmann::json& params)>;

    struct Handlers
    {
        // markup_list: the rows, newest change first; params as that method's.
        Request List;
        // markup_get: one mark-up with its shape, notes and thread; params {entityId}.
        Request Query;
        // markup_comment: adds a comment to the mark-up's thread; params {entityId, text, author}.
        Request Comment;
        // Selects the mark-up and frames it in the main Scene View, as a double-click on its
        // Mark-ups panel row does; params {entityId}; the result is empty.
        Request Frame;
    };

    static EditorMarkupRequests& Get();

    // Replaces the installed handlers; empty Handlers uninstalls them.
    void Install(Handlers handlers);

    MarkupRequestAnswer List(const nlohmann::json& params) const;
    MarkupRequestAnswer Query(ECS::EntityHandle markup) const;
    MarkupRequestAnswer Comment(ECS::EntityHandle markup, const std::string& text,
                                Components::MarkupAuthor author) const;
    MarkupRequestAnswer Frame(ECS::EntityHandle markup) const;

private:
    EditorMarkupRequests() = default;

    static MarkupRequestAnswer Call(const Request& request, const nlohmann::json& params);

    Handlers m_Handlers;
};

} // namespace GameEngine::Editor
