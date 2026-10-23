#pragma once

// A context menu backend that records the rows a menu is built with and the key each shows, for
// tests of what a menu offers.

#include "Platform/ContextMenu.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace EditorTesting
{

class RecordingContextMenu final : public GameEngine::INativeContextMenu
{
public:
    struct Row
    {
        std::string Title;
        uint32_t CommandId = 0;
        std::string Shortcut;
    };
    std::vector<Row> Rows;

    void Clear() override { Rows.clear(); }
    uint32_t AddSubMenu(uint32_t, const std::string&) override { return ++m_NextSubMenu; }
    void AddItem(uint32_t, const std::string& title, uint32_t commandId, uint32_t) override
    {
        Rows.push_back({title, commandId});
    }
    void AddSeparator(uint32_t) override {}
    void SetCommandHandler(CommandCallback) override {}
    void SetStateProvider(StateProviderCallback) override {}
    void SetItemEnabled(uint32_t, bool) override {}
    void SetItemChecked(uint32_t, bool) override {}
    void SetItemShortcut(uint32_t commandId, const std::string& keyText) override
    {
        for (Row& row : Rows)
        {
            if (row.CommandId == commandId)
                row.Shortcut = keyText;
        }
    }
    void Show(GameEngine::Platform::Window*, int, int) override {}

    // The command of the row titled `title`; 0 when the menu has no such row.
    uint32_t CommandOf(const std::string& title) const
    {
        const Row* row = Find(title);
        return row ? row->CommandId : 0;
    }

    // The key the row titled `title` shows; empty when it shows none or the menu has no such row.
    std::string ShortcutOf(const std::string& title) const
    {
        const Row* row = Find(title);
        return row ? row->Shortcut : std::string{};
    }

private:
    const Row* Find(const std::string& title) const
    {
        const auto it = std::find_if(Rows.begin(), Rows.end(), [&](const Row& row) { return row.Title == title; });
        return it == Rows.end() ? nullptr : &*it;
    }

    uint32_t m_NextSubMenu = 1000;
};

} // namespace EditorTesting
