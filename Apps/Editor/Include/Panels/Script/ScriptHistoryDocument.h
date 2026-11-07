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
            {
                document->ReconcileDiskText(diskText);
                return document;
            }
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
    /// Whether saving local edits requires confirmation of disk changes.
    bool HasDiskConflict() const { return m_HasDiskConflict; }
    void SetText(std::string text)
    {
        m_Text = std::move(text);
        if (!IsDirty())
            m_HasDiskConflict = false;
    }
    void MarkSaved()
    {
        m_SavedText = m_Text;
        m_HasDiskConflict = false;
    }
    /// Adopt disk changes while preserving unsaved local text.
    void ReconcileDiskText(const std::string& diskText)
    {
        if (diskText == m_SavedText)
            return;
        if (!IsDirty())
            m_Text = diskText;
        m_SavedText = diskText;
        m_HasDiskConflict = IsDirty();
        m_Changed.Invoke();
    }
    /// Acknowledge disk changes only if they match the last observed contents.
    bool AcceptDiskChanges(const std::string& expectedDiskText)
    {
        if (m_SavedText != expectedDiskText)
            return false;
        m_HasDiskConflict = false;
        m_Changed.Invoke();
        return true;
    }
    Event<>& Changed() { return m_Changed; }

private:
    std::string m_Text;
    std::string m_SavedText;
    bool m_HasDiskConflict = false;
    Event<> m_Changed;
};
} // namespace GameEngine::Editor
