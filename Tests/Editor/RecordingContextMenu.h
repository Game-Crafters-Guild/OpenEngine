#pragma once

// A context menu backend that records the rows a menu is built with, the key each shows and whether
// it is enabled, for tests of what a menu offers.

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
        std::string Tooltip;
        bool Enabled = true;
    };
    std::vector<Row> Rows;

    void Clear() override { Rows.clear(); }
    uint32_t AddSubMenu(uint32_t, const std::string&) override { return ++m_NextSubMenu; }
    void AddItem(uint32_t, const std::string& title, uint32_t commandId, uint32_t flags) override
    {
        Rows.push_back({title, commandId, {}, {}, (flags & GameEngine::MenuItemFlag_Disabled) == 0});
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
    void SetItemTooltip(uint32_t commandId, const std::string& text) override
    {
        for (Row& row : Rows)
        {
            if (row.CommandId == commandId)
                row.Tooltip = text;
        }
    }
    void Show(GameEngine::Platform::Window*, int, int) override {}

    // The command of the row titled `title`; 0 when the menu has no such row.
    uint32_t CommandOf(const std::string& title) const
    {
        const Row* row = Find(title);
        return row ? row->CommandId : 0;
    }

    // True when the row titled `title` is enabled; false when it is disabled or the menu has no such row.
    bool IsEnabled(const std::string& title) const
    {
        const Row* row = Find(title);
        return row && row->Enabled;
    }
    // The key the row titled `title` shows; empty when it shows none or the menu has no such row.
    std::string ShortcutOf(const std::string& title) const
    {
        const Row* row = Find(title);
        return row ? row->Shortcut : std::string{};
    }

    // The tooltip of the row titled `title`; empty when it has none or the menu has no such row.
    std::string TooltipOf(const std::string& title) const
    {
        const Row* row = Find(title);
        return row ? row->Tooltip : std::string{};
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
