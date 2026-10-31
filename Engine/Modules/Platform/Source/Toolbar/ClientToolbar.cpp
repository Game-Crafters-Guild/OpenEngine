#include "ClientToolbar.h"

#include "Platform/ContextMenu.h"
#include "Platform/Window.h"
#include "Types/FlatMap.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <cmath>
#include <utility>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace GameEngine
{
namespace
{
FlatMap<GLFWwindow*, ClientToolbar*> s_KeyOwners;

bool MatchesAccessKey(const std::string& title, int key)
{
    if (title.empty())
        return false;
    return static_cast<unsigned char>(ToUpperAscii(std::string_view(title).substr(0, 1)).front()) == key;
}
}

ClientToolbar::ClientToolbar() = default;
ClientToolbar::~ClientToolbar() { Uninstall(); }

bool ClientToolbar::Install(Platform::Window* window)
{
    Uninstall();
    if (!window)
        return false;
    if (window->GetGLFWHandle() && s_KeyOwners.Contains(window->GetGLFWHandle()))
        return false;
    m_Window = window;
    m_KeyWindow = window->GetGLFWHandle();
    if (m_KeyWindow)
    {
        s_KeyOwners.InsertOrAssign(m_KeyWindow, this);
        m_PreviousKeyCallback = glfwSetKeyCallback(m_KeyWindow, KeyCallback);
    }
    return true;
}

void ClientToolbar::Uninstall()
{
    ClosePopup();
    if (m_KeyWindow)
    {
        if (m_Window && m_Window->GetGLFWHandle() == m_KeyWindow)
            glfwSetKeyCallback(m_KeyWindow, m_PreviousKeyCallback);
        s_KeyOwners.Erase(m_KeyWindow);
    }
    m_KeyWindow = nullptr;
    m_PreviousKeyCallback = nullptr;
    m_Window = nullptr;
    m_Callback = {};
    m_Regions.clear();
    m_Height = 0;
    Clear();
}

void ClientToolbar::Clear()
{
    ClosePopup();
    m_TopMenus.clear();
    m_Menus.clear();
    m_NextMenuId = 1;
    ++m_Revision;
}

void ClientToolbar::SetCommandHandler(CommandCallback callback) { m_Callback = std::move(callback); }

void ClientToolbar::SetMenuFactory(MenuFactory factory)
{
    ClosePopup();
    m_Factory = std::move(factory);
}

ClientToolbar::Menu* ClientToolbar::FindMenu(uint32_t menuId)
{
    const auto found = std::find_if(m_Menus.begin(), m_Menus.end(),
                                    [menuId](const Menu& menu) { return menu.Id == menuId; });
    return found == m_Menus.end() ? nullptr : &*found;
}

uint32_t ClientToolbar::AddMenu(const std::string& title)
{
    const uint32_t id = m_NextMenuId++;
    m_Menus.push_back({id, 0, {}});
    m_TopMenus.push_back({id, title});
    ++m_Revision;
    return id;
}

uint32_t ClientToolbar::AddSubMenu(uint32_t parentMenuId, const std::string& title)
{
    Menu* parent = FindMenu(parentMenuId);
    if (!parent)
        return 0;
    const uint32_t id = m_NextMenuId++;
    parent->Entries.push_back({title, 0, id, {}});
    m_Menus.push_back({id, parentMenuId, {}});
    ++m_Revision;
    return id;
}

void ClientToolbar::AddItem(uint32_t parentMenuId, const std::string& title, uint32_t commandId)
{
    if (Menu* menu = FindMenu(parentMenuId))
    {
        menu->Entries.push_back({title, commandId, 0, {}});
        ++m_Revision;
    }
}

void ClientToolbar::SetItemIcon(uint32_t commandId, const std::string& imagePath)
{
    for (auto& menu : m_Menus)
        for (auto& entry : menu.Entries)
            if (entry.SubMenuId == 0 && entry.CommandId == commandId)
                entry.Icon = imagePath;
}

void ClientToolbar::UpdateItemTitle(uint32_t commandId, const std::string& title)
{
    for (auto& menu : m_Menus)
        for (auto& entry : menu.Entries)
            if (entry.SubMenuId == 0 && entry.CommandId == commandId)
                entry.Title = title;
}

void ClientToolbar::SetTitleBarLayout(int height, std::vector<TitleBarMenuRegion> regions)
{
    m_Height = height;
    m_Regions = std::move(regions);
    if (height == 0)
        ClosePopup();
}

void ClientToolbar::PopulatePopup(uint32_t menuId, uint32_t popupParent)
{
    Menu* menu = FindMenu(menuId);
    if (!menu)
        return;
    for (const auto& entry : menu->Entries)
    {
        if (entry.SubMenuId != 0)
        {
            const uint32_t child = m_Popup->AddSubMenu(popupParent, entry.Title);
            PopulatePopup(entry.SubMenuId, child);
            continue;
        }
        const auto tab = entry.Title.find('\t');
        m_Popup->AddItem(popupParent, entry.Title.substr(0, tab), entry.CommandId, MenuItemFlag_None);
        if (tab != std::string::npos)
            m_Popup->SetItemShortcut(entry.CommandId, entry.Title.substr(tab + 1));
        if (!entry.Icon.empty())
            m_Popup->SetItemIcon(entry.CommandId, entry.Icon);
    }
}

void ClientToolbar::ClosePopup()
{
    m_MenuOpen = false;
    m_CurrentMenu = 0;
    if (!m_Popup)
        return;
    m_Popup->SetCloseHandler({});
    m_Popup->SetCommandHandler({});
    m_Popup->Clear();
    m_Popup.reset();
}

void ClientToolbar::ShowTitleBarMenu(uint32_t menuId)
{
    if (!m_Window || !m_Factory || m_Height <= 0 || m_TopMenus.empty() || (menuId != 0 && !FindMenu(menuId)))
        return;
    ClosePopup();
    m_Popup = m_Factory();
    if (!m_Popup)
        return;
    if (menuId == 0)
    {
        for (const auto& menu : m_TopMenus)
            PopulatePopup(menu.Id, m_Popup->AddSubMenu(0, menu.Title));
    }
    else
        PopulatePopup(menuId, 0);
    m_Popup->SetCommandHandler([this](uint32_t commandId) {
        auto callback = m_Callback;
        if (callback)
            callback(commandId);
    });
    m_Popup->SetCloseHandler([this]() { m_MenuOpen = false; m_CurrentMenu = 0; });
    uint32_t anchorId = menuId;
    for (Menu* menu = FindMenu(anchorId); menu && menu->ParentId != 0; menu = FindMenu(anchorId))
        anchorId = menu->ParentId;
    auto anchor = std::find_if(m_Regions.begin(), m_Regions.end(),
                               [anchorId](const auto& region) { return region.Id == anchorId; });
    if (anchor == m_Regions.end())
        anchor = std::find_if(m_Regions.begin(), m_Regions.end(), [](const auto& region) { return region.Id == 0; });
    const float scale = m_Window->GetUiContentScale() > 0.0f ? m_Window->GetUiContentScale() : 1.0f;
    const int x = anchor == m_Regions.end() ? 0 : static_cast<int>(std::lround(anchor->X / scale));
    const int bottom = anchor == m_Regions.end() ? m_Height : anchor->Y + anchor->Height;
    m_CurrentMenu = menuId;
    m_MenuOpen = true;
    m_Popup->Show(m_Window, x, static_cast<int>(std::lround(bottom / scale)));
}

bool ClientToolbar::HandleKey(int key, int action, int mods)
{
    if (action != GLFW_PRESS || !m_Factory ||
        (mods & (GLFW_MOD_CONTROL | GLFW_MOD_SUPER)) != 0 || m_Height <= 0)
        return false;
    if (key == GLFW_KEY_F10 && mods == 0 && !m_TopMenus.empty())
    {
        if (m_MenuOpen)
            ClosePopup();
        else
            ShowTitleBarMenu(m_TopMenus.front().Id);
        return true;
    }
    if ((mods & GLFW_MOD_ALT) != 0)
    {
        for (const auto& menu : m_TopMenus)
            if (MatchesAccessKey(menu.Title, key))
            {
                ShowTitleBarMenu(menu.Id);
                return true;
            }
        return false;
    }
    if (!m_MenuOpen)
        return false;
    if (key == GLFW_KEY_ESCAPE)
    {
        ClosePopup();
        return true;
    }
    Menu* current = FindMenu(m_CurrentMenu);
    if (!current)
        return false;
    if (key == GLFW_KEY_LEFT && current->ParentId != 0)
    {
        ShowTitleBarMenu(current->ParentId);
        return true;
    }
    if ((key == GLFW_KEY_LEFT || key == GLFW_KEY_RIGHT) && current->ParentId == 0)
    {
        const auto found = std::find_if(m_TopMenus.begin(), m_TopMenus.end(),
                                        [this](const auto& menu) { return menu.Id == m_CurrentMenu; });
        if (found == m_TopMenus.end())
            return false;
        const auto index = static_cast<size_t>(found - m_TopMenus.begin());
        const size_t offset = key == GLFW_KEY_LEFT ? m_TopMenus.size() - 1 : 1;
        ShowTitleBarMenu(m_TopMenus[(index + offset) % m_TopMenus.size()].Id);
        return true;
    }
    const Entry* match = nullptr;
    for (const auto& entry : current->Entries)
        if (MatchesAccessKey(entry.Title, key))
        {
            if (match)
                return false;
            match = &entry;
        }
    if (!match)
        return false;
    if (match->SubMenuId != 0)
        ShowTitleBarMenu(match->SubMenuId);
    else
    {
        const uint32_t commandId = match->CommandId;
        auto callback = m_Callback;
        ClosePopup();
        if (commandId != 0 && callback)
            callback(commandId);
    }
    return true;
}

void ClientToolbar::KeyCallback(GLFWwindow* window, int key, int scanCode, int action, int mods)
{
    auto* owner = s_KeyOwners.Find(window);
    if (!owner)
        return;
    ClientToolbar* toolbar = *owner;
    if (!toolbar->HandleKey(key, action, mods) && toolbar->m_PreviousKeyCallback)
        toolbar->m_PreviousKeyCallback(window, key, scanCode, action, mods);
}
}
