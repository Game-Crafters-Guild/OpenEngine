#pragma once

#include "Events/Event.h"
#include "Types/FlatMap.h"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace GameEngine::Editor
{
/// Script text belongs to its document even when no editor view is bound to it.
class ScriptHistoryDocument
{
public:
    static std::shared_ptr<ScriptHistoryDocument> Open(const std::filesystem::path& path, const std::string& diskText)
    {
        static FlatMap<std::filesystem::path, std::weak_ptr<ScriptHistoryDocument>> documents;
        documents.EraseIf([](const auto&, const auto& document) { return document.expired(); });
        if (const auto* found = documents.Find(path))
        {
            if (auto document = found->lock())
                return document;
        }
        auto document = std::make_shared<ScriptHistoryDocument>();
        document->m_Text = diskText;
        document->m_SavedText = diskText;
        documents.InsertOrAssign(path, document);
        return document;
    }

    const std::string& GetText() const { return m_Text; }
    const std::string& GetSavedText() const { return m_SavedText; }
    bool IsDirty() const { return m_Text != m_SavedText; }
    void SetText(std::string text) { m_Text = std::move(text); }
    void MarkSaved() { m_SavedText = m_Text; }
    Event<>& Changed() { return m_Changed; }

private:
    std::string m_Text;
    std::string m_SavedText;
    Event<> m_Changed;
};
} // namespace GameEngine::Editor
