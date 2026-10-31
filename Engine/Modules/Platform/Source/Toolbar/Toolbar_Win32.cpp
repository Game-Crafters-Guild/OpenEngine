#include "Platform/Toolbar.h"
#include "Platform/Window.h"
#include "MenuIconBitmap_Win32.h"
#include "WindowFrame_Win32.h"
#include "Types/StringUtils.h"

#if defined(_WIN32)
#include <windows.h>
#include <windowsx.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <unordered_map>
#include <vector>
#include <algorithm>
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

static std::wstring BuildMnemonicTitle(const std::string& title)
{
    if (title.empty())
        return {};
    return ToWide("&" + ReplaceAll(title, "&", "&&"));
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

        // Create empty menu bar
        m_MenuBar = CreateMenu();
        SetMenu(m_Hwnd, m_MenuBar);
        DrawMenuBar(m_Hwnd);

        // Subclass window proc
        m_PrevProc = reinterpret_cast<WNDPROC>(GetWindowLongPtr(m_Hwnd, GWLP_WNDPROC));
        s_Instances[m_Hwnd] = this;
        SetWindowLongPtr(m_Hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Win32Toolbar::WndProcThunk));

        // Preserve client height (compensate for menu bar)
        RECT rcWin{}, rcClientBefore{}, rcClientAfter{};
        GetWindowRect(m_Hwnd, &rcWin);
        GetClientRect(m_Hwnd, &rcClientBefore);
        GetClientRect(m_Hwnd, &rcClientAfter);
        int beforeH = rcClientBefore.bottom - rcClientBefore.top;
        int afterH = rcClientAfter.bottom - rcClientAfter.top;
        if (afterH < beforeH)
        {
            int delta = beforeH - afterH;
            SetWindowPos(m_Hwnd, nullptr, rcWin.left, rcWin.top,
                         (rcWin.right - rcWin.left), (rcWin.bottom - rcWin.top) + delta,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }

        return true;
    }

    void Uninstall() override
    {
        if (m_Hwnd && m_PrevProc)
        {
            m_Frame.SetLayout(m_Hwnd, 0, {});
            SetWindowLongPtr(m_Hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(m_PrevProc));
        }
        if (m_MenuBar)
        {
            if (m_Hwnd)
                SetMenu(m_Hwnd, nullptr);
            DestroyMenu(m_MenuBar);
            m_MenuBar = nullptr;
        }
        if (m_Hwnd)
        {
            DrawMenuBar(m_Hwnd);
            s_Instances.erase(m_Hwnd);
        }
        m_Hwnd = nullptr;
        m_PrevProc = nullptr;
        m_Menus.clear();
        m_TopMenus.clear();
        m_Regions.clear();
        m_CustomCaption = false;
        m_Frame = {};
        m_Items.clear();
        ReleaseItemBitmaps();
        m_NextMenuId = 1;
        m_Callback = nullptr;
        ++m_MenuRevision;
    }

    void Clear() override
    {
        if (!m_Hwnd || !m_MenuBar)
            return;

        // Clear menu contents in-place without ever detaching the menu bar
        // from the window. Toggling the menu bar on/off with SetMenu(hwnd,
        // nullptr) changes the client area height and causes framebuffer resize
        // callbacks, which in turn trigger swapchain recreation. By mutating
        // the existing HMENU instead, we keep the non-client metrics stable.

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

        DrawMenuBar(m_Hwnd);

        m_Menus.clear();
        m_TopMenus.clear();
        ++m_MenuRevision;
        m_Items.clear();
        ReleaseItemBitmaps();
        m_NextMenuId = 1;
    }

    void SetCommandHandler(CommandCallback cb) override { m_Callback = std::move(cb); }

    uint32_t AddMenu(const std::string& title) override
    {
        if (!m_MenuBar)
            return 0;
        HMENU hMenu = CreatePopupMenu();
        AppendMenuW(m_MenuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(hMenu), BuildMnemonicTitle(title).c_str());
        DrawMenuBar(m_Hwnd);
        uint32_t id = m_NextMenuId++;
        m_Menus[id] = hMenu;
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
        AppendMenuW(parent, MF_POPUP, reinterpret_cast<UINT_PTR>(child), BuildMnemonicTitle(title).c_str());
        DrawMenuBar(m_Hwnd);
        uint32_t id = m_NextMenuId++;
        m_Menus[id] = child;
        return id;
    }

    void AddItem(uint32_t parentMenuId, const std::string& title, uint32_t commandId) override
    {
        HMENU parent = GetMenuById(parentMenuId);
        if (!parent)
            return;
        // Append as the last item and remember its position so we can
        // efficiently update the label in-place later without tearing down
        // the entire menu bar.
        int position = GetMenuItemCount(parent);
        AppendMenuW(parent, MF_STRING, commandId, BuildMnemonicTitle(title).c_str());
        m_Items[commandId] = {parent, static_cast<UINT>(position)};
        DrawMenuBar(m_Hwnd);
    }

    void SetItemIcon(uint32_t commandId, const std::string& imagePath) override
    {
        auto item = m_Items.find(commandId);
        if (item == m_Items.end() || imagePath.empty())
            return;

        const int size = Platform::MenuIconPixelSize();
        HBITMAP bitmap = Platform::CreateMenuIconBitmap(imagePath, size, size);
        if (!bitmap)
            return;

        auto old = m_ItemBitmaps.find(commandId);
        if (old != m_ItemBitmaps.end() && old->second)
            DeleteObject(old->second);
        m_ItemBitmaps[commandId] = bitmap;

        MENUITEMINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = MIIM_BITMAP;
        info.hbmpItem = bitmap;
        SetMenuItemInfoW(item->second.parent, item->second.position, TRUE, &info);
        DrawMenuBar(m_Hwnd);
    }

    void UpdateItemTitle(uint32_t commandId, const std::string& title) override
    {
        if (!m_Hwnd || !m_MenuBar)
            return;
        auto it = m_Items.find(commandId);
        if (it == m_Items.end())
            return;
        HMENU parent = it->second.parent;
        UINT position = it->second.position;
        if (!parent)
            return;

        std::wstring w = BuildMnemonicTitle(title);
        if (w.empty())
            return;

        MENUITEMINFOW info{};
        info.cbSize = sizeof(MENUITEMINFOW);
        info.fMask = MIIM_STRING;
        info.dwTypeData = const_cast<LPWSTR>(w.c_str());
        info.cch = static_cast<UINT>(w.size());
        if (!SetMenuItemInfoW(parent, position, TRUE, &info))
        {
            // Fallback for older systems: modify the item by position.
            ModifyMenuW(parent, position, MF_BYPOSITION | MF_STRING, commandId, w.c_str());
        }

        DrawMenuBar(m_Hwnd);
    }

    ToolbarPresentation GetPresentation() const override { return ToolbarPresentation::WindowTitleBar; }
    std::vector<ToolbarMenu> GetMenus() const override { return m_TopMenus; }
    uint64_t GetMenuRevision() const override { return m_MenuRevision; }
    bool IsMenuOpen() const override { return m_Tracking; }

    TitleBarMetrics GetTitleBarMetrics() const override
    {
        return m_Hwnd ? m_Frame.GetMetrics(m_Hwnd) : TitleBarMetrics{};
    }

    void SetTitleBarLayout(int height, std::vector<TitleBarMenuRegion> regions) override
    {
        if (!m_Hwnd)
            return;
        if (!m_CustomCaption)
        {
            // Keep the HMENU as the owner of the popup tree, without allocating
            // a second non-client row. The Editor paints the caption contents.
            m_CustomCaption = true;
            SetMenu(m_Hwnd, nullptr);
        }
        m_Regions = regions;
        m_Frame.SetLayout(m_Hwnd, height, std::move(regions));
    }

    void ShowTitleBarMenu(uint32_t menuId) override
    {
        if (!m_Hwnd || m_Tracking || m_TopMenus.empty())
            return;

        SendMessageW(m_Hwnd, WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEACCEL), 0);

        // WH_MSGFILTER observes only this thread's native menu loop. It does not
        // synthesize input or touch another application's pointer or focus.
        s_TrackingToolbar = this;
        HHOOK hook = SetWindowsHookExW(WH_MSGFILTER, MenuFilter, nullptr, GetCurrentThreadId());
        m_Tracking = true;
        uint32_t command = 0;
        do
        {
            m_CurrentMenu = menuId;
            m_NextMenu = kNoMenu;
            m_SelectedMenu = nullptr;
            m_SelectedFlags = 0;
            const auto region = std::find_if(m_Regions.begin(), m_Regions.end(),
                                             [menuId](const auto& entry) { return entry.Id == menuId; });
            // Hidden headings are reachable through the overflow tree.
            if (region == m_Regions.end())
            {
                menuId = 0;
                m_CurrentMenu = 0;
            }
            const auto anchor = std::find_if(m_Regions.begin(), m_Regions.end(),
                                             [menuId](const auto& entry) { return entry.Id == menuId; });
            POINT point{anchor != m_Regions.end() ? anchor->X : 0,
                        anchor != m_Regions.end() ? anchor->Y + anchor->Height : m_Frame.GetMetrics(m_Hwnd).Height};
            ClientToScreen(m_Hwnd, &point);
            HMENU overflow = nullptr;
            if (menuId == 0)
            {
                overflow = CreatePopupMenu();
                for (const auto& menu : m_TopMenus)
                    AppendMenuW(overflow, MF_POPUP, reinterpret_cast<UINT_PTR>(GetMenuById(menu.Id)),
                                BuildMnemonicTitle(menu.Title).c_str());
            }
            m_ActivePopup = overflow ? overflow : GetMenuById(menuId);
            command = TrackPopupMenuEx(m_ActivePopup, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD,
                                       point.x, point.y, m_Hwnd, nullptr);
            if (overflow)
            {
                // These submenus belong to m_MenuBar, not the temporary popup.
                while (GetMenuItemCount(overflow) > 0)
                    RemoveMenu(overflow, 0, MF_BYPOSITION);
                DestroyMenu(overflow);
            }
            menuId = m_NextMenu;
        } while (menuId != kNoMenu && command == 0);
        if (hook)
            UnhookWindowsHookEx(hook);
        s_TrackingToolbar = nullptr;
        m_ActivePopup = nullptr;
        m_Tracking = false;
        // Callbacks can rebuild the tree, so dispatch only after the menu loop
        // and hook have stopped using its native handles and item bitmaps.
        if (command != 0 && m_Callback)
            m_Callback(command);
    }

  private:
    static constexpr uint32_t kNoMenu = UINT32_MAX;

    void SwitchMenu(int direction)
    {
        if (m_Regions.size() < 2)
            return;
        const auto current = std::find_if(m_Regions.begin(), m_Regions.end(),
                                          [this](const auto& entry) { return entry.Id == m_CurrentMenu; });
        const int count = static_cast<int>(m_Regions.size());
        const int index = current == m_Regions.end() ? 0 : static_cast<int>(current - m_Regions.begin());
        m_NextMenu = m_Regions[static_cast<size_t>((index + direction + count) % count)].Id;
        EndMenu();
    }

    static LRESULT CALLBACK MenuFilter(int code, WPARAM wParam, LPARAM lParam)
    {
        auto* self = s_TrackingToolbar;
        if (code == MSGF_MENU && self)
        {
            const auto& message = *reinterpret_cast<MSG*>(lParam);
            if (message.message == WM_MOUSEMOVE)
            {
                POINT point = message.pt;
                ScreenToClient(self->m_Hwnd, &point);
                for (const auto& region : self->m_Regions)
                {
                    if (point.x >= region.X && point.x < region.X + region.Width &&
                        point.y >= region.Y && point.y < region.Y + region.Height && region.Id != self->m_CurrentMenu)
                    {
                        self->m_NextMenu = region.Id;
                        EndMenu();
                        return TRUE;
                    }
                }
            }
            if (message.message == WM_KEYDOWN && self->m_CurrentMenu != 0 &&
                (self->m_SelectedMenu == self->m_ActivePopup || self->m_SelectedMenu == nullptr))
            {
                if (message.wParam == VK_LEFT ||
                    (message.wParam == VK_RIGHT && !(self->m_SelectedFlags & MF_POPUP)))
                {
                    self->SwitchMenu(message.wParam == VK_LEFT ? -1 : 1);
                    return TRUE;
                }
            }
        }
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    struct ItemBinding
    {
        HMENU parent;
        UINT position;
    };

    HMENU GetMenuById(uint32_t id) const
    {
        auto it = m_Menus.find(id);
        return it == m_Menus.end() ? nullptr : it->second;
    }

    void ReleaseItemBitmaps()
    {
        for (const auto& [_, bitmap] : m_ItemBitmaps)
            if (bitmap)
                DeleteObject(bitmap);
        m_ItemBitmaps.clear();
    }

    static LRESULT CALLBACK WndProcThunk(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        auto it = s_Instances.find(hWnd);
        Win32Toolbar* self = (it != s_Instances.end()) ? it->second : nullptr;
        if (self)
            return self->WndProc(hWnd, msg, wParam, lParam);
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }

    LRESULT WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == WM_NCDESTROY)
        {
            const WNDPROC previous = m_PrevProc;
            s_Instances.erase(hWnd);
            m_Hwnd = nullptr;
            m_PrevProc = nullptr;
            return CallWindowProcW(previous, hWnd, msg, wParam, lParam);
        }
        if (msg == WM_MENUSELECT && m_Tracking)
        {
            m_SelectedMenu = reinterpret_cast<HMENU>(lParam);
            m_SelectedFlags = HIWORD(wParam);
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
    HMENU m_MenuBar = nullptr;
    WNDPROC m_PrevProc = nullptr;
    std::unordered_map<uint32_t, HMENU> m_Menus;
    std::unordered_map<uint32_t, ItemBinding> m_Items;
    std::unordered_map<uint32_t, HBITMAP> m_ItemBitmaps;
    uint32_t m_NextMenuId = 1;
    CommandCallback m_Callback;
    Platform::WindowFrame m_Frame;
    std::vector<ToolbarMenu> m_TopMenus;
    std::vector<TitleBarMenuRegion> m_Regions;
    uint64_t m_MenuRevision = 0;
    bool m_CustomCaption = false;
    bool m_Tracking = false;
    uint32_t m_CurrentMenu = 0;
    uint32_t m_NextMenu = kNoMenu;
    HMENU m_ActivePopup = nullptr;
    HMENU m_SelectedMenu = nullptr;
    UINT m_SelectedFlags = 0;

    static inline std::unordered_map<HWND, Win32Toolbar*> s_Instances;
    static inline thread_local Win32Toolbar* s_TrackingToolbar = nullptr;
};

std::unique_ptr<INativeToolbar> CreateNativeToolbar()
{
    return std::make_unique<Win32Toolbar>();
}

} // namespace GameEngine

#endif // _WIN32
