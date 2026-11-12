#pragma once

#include "Platform/ContextMenu.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine::Tests
{
// What a toolbar built into its popup and where it showed it.
struct PopupRecord
{
    struct Row
    {
        uint32_t Parent = 0;
        std::string Title;
        uint32_t Command = 0;
        uint32_t Submenu = 0;
        bool Separator = false;
    };
    std::vector<Row> Rows;
    std::string Icon;
    std::string Shortcut;
    INativeContextMenu::CommandCallback Command;
    std::function<void()> Closed;
    // Show closes the popup before it returns, as a native popup's modal menu loop does.
    bool ClosesInShow = false;
    Platform::Window* Window = nullptr;
    int X = 0;
    int Y = 0;
    int Shows = 0;
    int Clears = 0;
};

// A popup that records its rows and its Show call instead of opening.
class RecordingPopup final : public INativeContextMenu
{
  public:
    explicit RecordingPopup(std::shared_ptr<PopupRecord> record) : m_Record(std::move(record)) {}
    void Clear() override { ++m_Record->Clears; m_Record->Rows.clear(); }
    uint32_t AddSubMenu(uint32_t parent, const std::string& title) override
    {
        const uint32_t id = ++m_NextId;
        m_Record->Rows.push_back({parent, title, 0, id});
        return id;
    }
    void AddItem(uint32_t parent, const std::string& title, uint32_t command, uint32_t) override
    {
        m_Record->Rows.push_back({parent, title, command, 0});
    }
    void AddSeparator(uint32_t parent) override { m_Record->Rows.push_back({parent, {}, 0, 0, true}); }
    void SetCommandHandler(CommandCallback callback) override { m_Record->Command = std::move(callback); }
    void SetCloseHandler(std::function<void()> callback) override { m_Record->Closed = std::move(callback); }
    void SetStateProvider(StateProviderCallback) override {}
    void SetItemEnabled(uint32_t, bool) override {}
    void SetItemChecked(uint32_t, bool) override {}
    void SetItemIcon(uint32_t, const std::string& icon) override { m_Record->Icon = icon; }
    void SetItemShortcut(uint32_t, const std::string& shortcut) override { m_Record->Shortcut = shortcut; }
    void Show(Platform::Window* window, int x, int y) override
    {
        m_Record->Window = window;
        m_Record->X = x;
        m_Record->Y = y;
        ++m_Record->Shows;
        if (m_Record->ClosesInShow && m_Record->Closed)
            m_Record->Closed();
    }
  private:
    std::shared_ptr<PopupRecord> m_Record;
    uint32_t m_NextId = 100;
};
} // namespace GameEngine::Tests
