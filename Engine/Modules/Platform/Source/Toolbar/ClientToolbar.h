#pragma once

#include "Platform/Toolbar.h"

struct GLFWwindow;

namespace GameEngine
{

/// Menu model for hosts that keep compositor decorations and draw menus in their client area.
/// The window object and GLFW event thread must outlive the installation.
class ClientToolbar final : public INativeToolbar
{
  public:
    ClientToolbar();
    ~ClientToolbar() override;
    bool Install(Platform::Window* window) override;
    void Uninstall() override;
    void Clear() override;
    void SetCommandHandler(CommandCallback callback) override;
    void SetMenuFactory(MenuFactory factory) override;
    uint32_t AddMenu(const std::string& title) override;
    uint32_t AddSubMenu(uint32_t parentMenuId, const std::string& title) override;
    void AddItem(uint32_t parentMenuId, const std::string& title, uint32_t commandId) override;
    void SetItemIcon(uint32_t commandId, const std::string& imagePath) override;
    void UpdateItemTitle(uint32_t commandId, const std::string& title) override;
    ToolbarPresentation GetPresentation() const override { return ToolbarPresentation::ClientMenuBar; }
    std::vector<ToolbarMenu> GetMenus() const override { return m_TopMenus; }
    uint64_t GetMenuRevision() const override { return m_Revision; }
    void SetTitleBarLayout(int height, std::vector<TitleBarMenuRegion> regions) override;
    void ShowTitleBarMenu(uint32_t menuId) override;
    bool IsMenuOpen() const override { return m_MenuOpen; }

  private:
    struct Entry
    {
        std::string Title;
        uint32_t CommandId = 0;
        uint32_t SubMenuId = 0;
        std::string Icon;
    };
    struct Menu
    {
        uint32_t Id = 0;
        uint32_t ParentId = 0;
        std::vector<Entry> Entries;
    };

    Menu* FindMenu(uint32_t menuId);
    void PopulatePopup(uint32_t menuId, uint32_t popupParent);
    void ClosePopup();
    bool HandleKey(int key, int action, int mods);
    static void KeyCallback(GLFWwindow* window, int key, int scanCode, int action, int mods);

    Platform::Window* m_Window = nullptr;
    GLFWwindow* m_KeyWindow = nullptr;
    void (*m_PreviousKeyCallback)(GLFWwindow*, int, int, int, int) = nullptr;
    CommandCallback m_Callback;
    MenuFactory m_Factory;
    std::unique_ptr<INativeContextMenu> m_Popup;
    std::vector<ToolbarMenu> m_TopMenus;
    std::vector<Menu> m_Menus;
    std::vector<TitleBarMenuRegion> m_Regions;
    uint32_t m_NextMenuId = 1;
    uint32_t m_CurrentMenu = 0;
    uint64_t m_Revision = 0;
    int m_Height = 0;
    bool m_MenuOpen = false;
};
}
