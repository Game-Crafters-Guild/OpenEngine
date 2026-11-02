#include "Platform/Toolbar.h"
#include "Platform/ContextMenu.h"
#include "Platform/Window.h"
#include "WindowFrame_Win32.h"
#include "Types/FlatMap.h"

#if defined(_WIN32)
#include <windows.h>
#include <windowsx.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cwctype>

namespace GameEngine
{

static std::wstring ToWide(const std::string& s)
{
    if (s.empty())
        return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], len);
    return w;
}

class Win32Toolbar : public INativeToolbar
{
  public:
    ~Win32Toolbar() override { Uninstall(); }

    bool Install(Platform::Window* window) override
    {
        Uninstall();
        if (!window)
            return false;
        GLFWwindow* gw = window->GetGLFWHandle();
        if (!gw)
            return false;
        m_Hwnd = glfwGetWin32Window(gw);
        if (!m_Hwnd)
            return false;
        m_Window = window;

        // The menu bar owns the popup tree and never attaches to the window: a native
        // menu row would change the client rect each time it appears or empties.
        m_MenuBar = CreateMenu();

        m_PrevProc = reinterpret_cast<WNDPROC>(GetWindowLongPtr(m_Hwnd, GWLP_WNDPROC));
        s_Instances.InsertOrAssign(m_Hwnd, this);
        SetWindowLongPtr(m_Hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Win32Toolbar::WndProcThunk));
        // The custom frame installs now, so the client rect is final before a swapchain reads it.
        // The first title bar layout then changes only the caption height and the menu regions.
        m_Frame.SetLayout(m_Hwnd, m_Frame.GetMetrics(m_Hwnd).Height, {});
        return true;
    }

    void Uninstall() override
    {
        ClosePopup();
        if (m_Hwnd && m_PrevProc)
        {
            m_Frame.SetLayout(m_Hwnd, 0, {});
            SetWindowLongPtr(m_Hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(m_PrevProc));
        }
        if (m_MenuBar)
        {
            DestroyMenu(m_MenuBar);
            m_MenuBar = nullptr;
        }
        if (m_Hwnd)
            s_Instances.Erase(m_Hwnd);
        m_Hwnd = nullptr;
        m_Window = nullptr;
        m_PrevProc = nullptr;
        m_Menus.Clear();
        m_TopMenus.clear();
        m_Regions.clear();
        m_SubMenuTitles.Clear();
        m_ItemTexts.Clear();
        m_CustomCaption = false;
        m_Frame = {};
        m_NextMenuId = 1;
        m_Callback = nullptr;
        ++m_MenuRevision;
    }

    void Clear() override
    {
        if (!m_Hwnd || !m_MenuBar)
            return;
        ClosePopup();

        // Destroy and remove all top-level menus and their submenus.
        int count = GetMenuItemCount(m_MenuBar);
        for (int i = count - 1; i >= 0; --i)
        {
            HMENU sub = GetSubMenu(m_MenuBar, i);
            // Remove the item from the bar first.
            RemoveMenu(m_MenuBar, static_cast<UINT>(i), MF_BYPOSITION);
            if (sub)
            {
                // Destroy the submenu hierarchy to avoid leaks.
                DestroyMenu(sub);
            }
        }

        m_Menus.Clear();
        m_TopMenus.clear();
        m_SubMenuTitles.Clear();
        m_ItemTexts.Clear();
        ++m_MenuRevision;
        m_NextMenuId = 1;
    }

    void SetCommandHandler(CommandCallback cb) override { m_Callback = std::move(cb); }

    void SetMenuFactory(MenuFactory factory) override
    {
        ClosePopup();
        m_Factory = std::move(factory);
    }

    uint32_t AddMenu(const std::string& title) override
    {
        if (!m_MenuBar)
            return 0;
        HMENU hMenu = CreatePopupMenu();
        AppendMenuW(m_MenuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(hMenu), ToWide(title).c_str());
        uint32_t id = m_NextMenuId++;
        m_Menus.InsertOrAssign(id, hMenu);
        m_TopMenus.push_back({id, title});
        ++m_MenuRevision;
        return id;
    }

    uint32_t AddSubMenu(uint32_t parentMenuId, const std::string& title) override
    {
        HMENU parent = GetMenuById(parentMenuId);
        if (!parent)
            return 0;
        HMENU child = CreatePopupMenu();
        AppendMenuW(parent, MF_POPUP, reinterpret_cast<UINT_PTR>(child), ToWide(title).c_str());
        uint32_t id = m_NextMenuId++;
        m_Menus.InsertOrAssign(id, child);
        m_SubMenuTitles.InsertOrAssign(child, title);
        return id;
    }

    void AddItem(uint32_t parentMenuId, const std::string& title, uint32_t commandId) override
    {
        HMENU parent = GetMenuById(parentMenuId);
        if (!parent)
            return;
        AppendMenuW(parent, MF_STRING, commandId, ToWide(title).c_str());
        m_ItemTexts.InsertOrAssign(commandId, ItemText{title, {}});
    }

    void AddSeparator(uint32_t parentMenuId) override
    {
        HMENU parent = GetMenuById(parentMenuId);
        if (!parent)
            return;
        AppendMenuW(parent, MF_SEPARATOR, 0, nullptr);
    }

    // The popup reads the title and the icon when it opens.
    void SetItemIcon(uint32_t commandId, const std::string& imagePath) override
    {
        if (imagePath.empty())
            return;
        if (ItemText* text = m_ItemTexts.Find(commandId))
            text->Icon = imagePath;
    }

    void UpdateItemTitle(uint32_t commandId, const std::string& title) override
    {
        if (title.empty())
            return;
        if (ItemText* text = m_ItemTexts.Find(commandId))
            text->Title = title;
    }

    ToolbarPresentation GetPresentation() const override { return ToolbarPresentation::WindowTitleBar; }
    std::vector<ToolbarMenu> GetMenus() const override { return m_TopMenus; }
    uint64_t GetMenuRevision() const override { return m_MenuRevision; }
    std::optional<uint32_t> GetOpenMenu() const override { return m_OpenMenu; }

    TitleBarMetrics GetTitleBarMetrics() const override
    {
        return m_Hwnd ? m_Frame.GetMetrics(m_Hwnd) : TitleBarMetrics{};
    }

    void SetTitleBarLayout(int height, std::vector<TitleBarMenuRegion> regions) override
    {
        if (!m_Hwnd)
            return;
        // The Editor paints the caption contents and opens the menus from them.
        m_CustomCaption = true;
        if (height == 0)
            ClosePopup();
        m_Regions = regions;
        m_Frame.SetLayout(m_Hwnd, height, std::move(regions));
    }

    void ShowTitleBarMenu(uint32_t menuId) override
    {
        if (!m_Hwnd || !m_Window || !m_Factory || m_TopMenus.empty())
            return;
        auto anchor = std::find_if(m_Regions.begin(), m_Regions.end(),
                                   [menuId](const auto& region) { return region.Id == menuId; });
        // A hidden heading opens through the overflow tree.
        if (anchor == m_Regions.end())
        {
            menuId = 0;
            anchor = std::find_if(m_Regions.begin(), m_Regions.end(), [](const auto& region) { return region.Id == 0; });
        }
        ClosePopup();
        m_Popup = m_Factory();
        if (!m_Popup)
            return;
        if (menuId == 0)
        {
            for (const auto& menu : m_TopMenus)
                PopulatePopup(GetMenuById(menu.Id), m_Popup->AddSubMenu(0, menu.Title));
        }
        else
            PopulatePopup(GetMenuById(menuId), 0);
        m_Popup->SetCommandHandler([this](uint32_t commandId) {
            auto callback = m_Callback;
            if (callback)
                callback(commandId);
        });
        m_Popup->SetCloseHandler([this]() { m_OpenMenu.reset(); });
        // Regions are in physical client pixels; the popup takes logical pixels.
        const float scale = m_Window->GetUiContentScale() > 0.0f ? m_Window->GetUiContentScale() : 1.0f;
        const int x = anchor == m_Regions.end() ? 0 : anchor->Bounds.X;
        const int bottom = anchor == m_Regions.end() ? m_Frame.GetMetrics(m_Hwnd).Height : anchor->Bounds.Bottom();
        m_OpenMenu = menuId;
        m_Popup->Show(m_Window, static_cast<int>(std::lround(x / scale)), static_cast<int>(std::lround(bottom / scale)));
    }

  private:
    struct ItemText
    {
        std::string Title;
        std::string Icon;
    };

    // Copies one native menu level into the popup. The titles, shortcuts and icons
    // come from the UTF-8 maps.
    void PopulatePopup(HMENU menu, uint32_t popupParent)
    {
        const int count = menu ? GetMenuItemCount(menu) : 0;
        for (int index = 0; index < count; ++index)
        {
            MENUITEMINFOW info{};
            info.cbSize = sizeof(info);
            info.fMask = MIIM_FTYPE | MIIM_ID | MIIM_SUBMENU;
            if (!GetMenuItemInfoW(menu, static_cast<UINT>(index), TRUE, &info))
                continue;
            if (info.fType & MFT_SEPARATOR)
            {
                m_Popup->AddSeparator(popupParent);
                continue;
            }
            if (info.hSubMenu)
            {
                const std::string* title = m_SubMenuTitles.Find(info.hSubMenu);
                PopulatePopup(info.hSubMenu, m_Popup->AddSubMenu(popupParent, title ? *title : std::string()));
                continue;
            }
            const ItemText* text = m_ItemTexts.Find(info.wID);
            if (!text)
                continue;
            const auto tab = text->Title.find('\t');
            m_Popup->AddItem(popupParent, text->Title.substr(0, tab), info.wID, MenuItemFlag_None);
            if (tab != std::string::npos)
                m_Popup->SetItemShortcut(info.wID, text->Title.substr(tab + 1));
            if (!text->Icon.empty())
                m_Popup->SetItemIcon(info.wID, text->Icon);
        }
    }

    void ClosePopup()
    {
        m_OpenMenu.reset();
        if (!m_Popup)
            return;
        m_Popup->SetCloseHandler({});
        m_Popup->SetCommandHandler({});
        m_Popup->Clear();
        m_Popup.reset();
    }

    HMENU GetMenuById(uint32_t id) const
    {
        const HMENU* menu = m_Menus.Find(id);
        return menu ? *menu : nullptr;
    }

    static LRESULT CALLBACK WndProcThunk(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (Win32Toolbar* const* self = s_Instances.Find(hWnd))
            return (*self)->WndProc(hWnd, msg, wParam, lParam);
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }

    LRESULT WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == WM_NCDESTROY)
        {
            const WNDPROC previous = m_PrevProc;
            s_Instances.Erase(hWnd);
            m_Hwnd = nullptr;
            m_PrevProc = nullptr;
            return CallWindowProcW(previous, hWnd, msg, wParam, lParam);
        }
        if (m_CustomCaption && msg == WM_SYSCOMMAND && (wParam & 0xFFF0) == SC_KEYMENU)
        {
            // GLFW suppresses SC_KEYMENU by default. Alt+Space belongs to the
            // system menu, independently of the Editor's application menus.
            if (lParam == VK_SPACE)
                return DefWindowProcW(hWnd, msg, wParam, lParam);
            if (m_TopMenus.empty())
                return 0;
            uint32_t menuId = m_TopMenus.front().Id;
            if (lParam != 0)
            {
                const wint_t mnemonic = towupper(static_cast<wint_t>(lParam));
                for (const auto& menu : m_TopMenus)
                {
                    const auto title = ToWide(menu.Title);
                    if (!title.empty() && towupper(title.front()) == mnemonic)
                    {
                        menuId = menu.Id;
                        break;
                    }
                }
            }
            ShowTitleBarMenu(menuId);
            return 0;
        }
        LRESULT frameResult = 0;
        if (msg == WM_GETMINMAXINFO)
        {
            // Preserve GLFW's own size limits and monitor work-area handling.
            frameResult = CallWindowProcW(m_PrevProc, hWnd, msg, wParam, lParam);
            m_Frame.HandleMessage(hWnd, msg, wParam, lParam, frameResult);
            return frameResult;
        }
        if (m_Frame.HandleMessage(hWnd, msg, wParam, lParam, frameResult))
            return frameResult;
        if (msg == WM_COMMAND)
        {
            uint16_t cmd = LOWORD(wParam);
            if (m_Callback)
            {
                m_Callback(static_cast<uint32_t>(cmd));
                return 0;
            }
        }
        return m_PrevProc ? CallWindowProc(m_PrevProc, hWnd, msg, wParam, lParam) : DefWindowProc(hWnd, msg, wParam, lParam);
    }

    HWND m_Hwnd = nullptr;
    Platform::Window* m_Window = nullptr;
    HMENU m_MenuBar = nullptr;
    WNDPROC m_PrevProc = nullptr;
    FlatMap<uint32_t, HMENU> m_Menus;
    uint32_t m_NextMenuId = 1;
    CommandCallback m_Callback;
    MenuFactory m_Factory;
    std::unique_ptr<INativeContextMenu> m_Popup;
    FlatMap<HMENU, std::string> m_SubMenuTitles;
    FlatMap<uint32_t, ItemText> m_ItemTexts;
    Platform::WindowFrame m_Frame;
    std::vector<ToolbarMenu> m_TopMenus;
    std::vector<TitleBarMenuRegion> m_Regions;
    uint64_t m_MenuRevision = 0;
    bool m_CustomCaption = false;
    std::optional<uint32_t> m_OpenMenu;

    static inline FlatMap<HWND, Win32Toolbar*> s_Instances;
};

std::unique_ptr<INativeToolbar> CreateNativeToolbar()
{
    return std::make_unique<Win32Toolbar>();
}

} // namespace GameEngine

#endif // _WIN32
