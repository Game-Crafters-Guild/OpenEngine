#include "UI/Controls/EditorTitleBar.h"

#include "AssetCore/AssetTypes.h"
#include "Assets/AssetManager.h"
#include "Core/Engine.h"
#include "Input/InputSystem.h"
#include "Logger/Logger.h"
#include "Platform/Toolbar.h"
#include "Platform/Window.h"
#include "Rendering/Text/FontAtlas.h"
#include "Rendering/Text/TextLayout.h"
#include "UI/Assets/UILayoutAsset.h"
#include "UI/Assets/UIStyleAsset.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/ElementBuilder.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/StyleProperties.h"
#include "UI/UIManager.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace GameEngine
{
namespace
{
// A caption must remain grabbable even when menu contributions fill the row.
constexpr float kMinimumDragWidth = 64.0f;
constexpr float kMinimumHeight = 32.0f;
// Space between the last menu heading and the centered title.
constexpr float kTitleGap = 12.0f;
// ChromeMinimize, ChromeMaximize, ChromeRestore and ChromeClose from the Windows system icon font, as UTF-8.
constexpr const char* kMinimizeGlyph = "\xEE\xA4\xA1";
constexpr const char* kMaximizeGlyph = "\xEE\xA4\xA2";
constexpr const char* kRestoreGlyph = "\xEE\xA4\xA3";
constexpr const char* kCloseGlyph = "\xEE\xA2\xBB";
constexpr const char* kAccessKeysClass = "access-keys";
constexpr const char* kOpenClass = "open";

void SetClassPresent(UIElement* element, const char* className, bool present)
{
    if (!element || element->HasClass(className) == present)
        return;
    if (present)
        element->AddClass(className);
    else
        element->RemoveClass(className);
}

int Pixels(float logical, float scale)
{
    return static_cast<int>(std::lround(logical * scale));
}
} // namespace

EditorTitleBar::EditorTitleBar()
{
    SetId("EditorTitleBar");
    AddClass("editor-title-bar");
    // Hidden until its layout loads, and on platforms with a native menu bar (macOS).
    Style().Set(Style::Display, DisplayMode::None);
}

EditorTitleBar::~EditorTitleBar() = default;

bool EditorTitleBar::LoadAssets()
{
    if (m_AssetsLoaded)
        return true;
    UIManager* ui = GetOwnerManager();
    if (!ui)
        return false;
    auto& assets = EngineCore::GetInstance().GetAssetManager();
    const GUID layoutGuid = assets.ResolveAssetGuid("UI/controls/EditorTitleBar.uxml", "Editor");
    const GUID styleGuid = assets.ResolveAssetGuid("UI/controls/EditorTitleBar.css", "Editor");
    if (layoutGuid.IsNull() || styleGuid.IsNull())
        return false;
    // Waits on purpose: the editor's own .uxml and .css, small and never cooked, read once at startup.
    auto layout = assets.LoadAssetAsync(layoutGuid, AssetLoadPriority::High).get();
    auto style = assets.LoadAssetAsync(styleGuid, AssetLoadPriority::High).get();
    if (!layout || layout->GetType() != AssetType::UILayout || !style || style->GetType() != AssetType::UIStyle ||
        !ui->BindLayoutToSubtreeChildrenFromAsset(this, *static_cast<UILayoutAsset*>(layout.get())) ||
        !ui->AttachStyleToSubtreeFromAsset(this, *static_cast<UIStyleAsset*>(style.get())))
        return false;
    m_Icon = FindById<UIElement>("title-bar-icon"_sid);
    m_Title = FindById<Label>("title-bar-title"_sid);
    m_MenusHost = FindById<UIElement>("title-bar-menus"_sid);
    m_Overflow = FindById<Button>("title-bar-overflow"_sid);
    m_CaptionLabels = {FindById<Label>("title-bar-minimize"_sid), FindById<Label>("title-bar-maximize"_sid),
                       FindById<Label>("title-bar-close"_sid)};
    if (auto* overflow = m_Overflow.Get())
        overflow->SetOnClick([this](UIEvent&) { OpenMenu(0); });
    m_AssetsLoaded = true;
    return true;
}

void EditorTitleBar::Bind(INativeToolbar* toolbar, Platform::Window* window)
{
    m_Toolbar = toolbar;
    m_Window = window;
    m_PushedHeight = -1;
    m_PushedRegions.clear();
    if (!toolbar || !window || toolbar->GetPresentation() == ToolbarPresentation::NativeMenuBar)
    {
        Style().Set(Style::Display, DisplayMode::None);
        return;
    }
    if (!LoadAssets())
    {
        Logger::Log::Error("EditorTitleBar: failed to load UI/controls/EditorTitleBar.uxml or .css; the window shows its system title bar");
        // Height 0 removes the custom frame that the toolbar installed.
        toolbar->SetTitleBarLayout(0, {});
        m_Toolbar = nullptr;
        m_Window = nullptr;
    }
}

void EditorTitleBar::UpdateState(const std::string& caption)
{
    if (!m_AssetsLoaded || !m_Toolbar || !m_Window ||
        m_Toolbar->GetPresentation() == ToolbarPresentation::NativeMenuBar)
        return;
    const bool fullscreen = m_Window->IsFullscreen();
    Style().Set(Style::Display, fullscreen ? DisplayMode::None : DisplayMode::Flex);
    if (fullscreen)
    {
        PushTitleBarLayout(0, {});
        return;
    }
    // The access-key underlines show only while Alt is held.
    const UIManager* ui = GetOwnerManager();
    const bool accessKeysShown = ui && (ui->GetModifierKeys() & Input::kModAlt) != 0;
    SetClassPresent(this, kAccessKeysClass, accessKeysShown);
    if (auto* title = m_Title.Get())
    {
        if (title->GetText() != caption)
            title->SetText(caption);
    }
    const std::optional<uint32_t> openMenu = m_Toolbar->GetOpenMenu();
    if (m_MenuRevision != m_Toolbar->GetMenuRevision() && !openMenu)
        RebuildMenus();
    // The heading of the open dropdown keeps its plate until the dropdown closes.
    for (const auto& heading : m_MenuHeadings)
        SetClassPresent(heading.Control.Get(), kOpenClass, openMenu == heading.Id);
    SetClassPresent(m_Overflow.Get(), kOpenClass, openMenu == 0u);
    // Update even without a layout solve: DPI and native caption metrics can
    // change while the CSS geometry and the scene title stay the same.
    OnPostLayout();
}

void EditorTitleBar::RebuildMenus()
{
    UIElement* host = m_MenusHost.Get();
    if (!host)
        return;
    host->RemoveAllChildren();
    m_MenuHeadings.clear();
    for (const auto& menu : m_Toolbar->GetMenus())
    {
        auto button = UI::Element<Button>()
            .Id("title-bar-menu-" + std::to_string(menu.Id))
            .Class("title-bar-menu")
            .Set(&Button::SetText, menu.Title)
            .Set(&Button::SetOnClick, [this, id = menu.Id](UIEvent&) { OpenMenu(id); })
            .Children(UI::Element<UIElement>().Class("title-bar-mnemonic"))
            .Build();
        if (!button)
            return;
        m_MenuHeadings.push_back({MakeWeakRef(button.get()), menu.Id});
        host->AddChild(std::move(button));
    }
    m_MenuRevision = m_Toolbar->GetMenuRevision();
}

void EditorTitleBar::UpdateMnemonicLayout(MenuHeading& heading)
{
    Button* button = heading.Control.Get();
    if (!button)
        return;
    UIManager* ui = GetOwnerManager();
    Label* label = nullptr;
    UIElement* underline = nullptr;
    for (const auto& child : button->GetChildren())
    {
        if (auto* text = dynamic_cast<Label*>(child.get()))
            label = text;
        if (child->HasClass("title-bar-mnemonic"_sid))
            underline = child.get();
    }
    if (!ui || !label || !underline)
        return;
    const auto& visual = label->GetResolvedStyle().Visual;
    auto* font = ui->ResolveFontForStyle(label->GetResolvedStyle());
    if (!font)
        return;
    const float scale = std::max(0.01f, ui->GetContentScale());
    const float pixelSize = std::max(1.0f, visual.FontSize * scale);
    const float letterSpacing = visual.LetterSpacing * scale;
    if (heading.Font != font || heading.PixelSize != pixelSize || heading.LetterSpacing != letterSpacing)
    {
        const auto measured = font->MeasureText(label->GetText(), pixelSize, letterSpacing);
        const auto end = std::find_if(measured.caretXByByte.begin(), measured.caretXByByte.end(),
                                      [](float position) { return position > 0.0f; });
        heading.MnemonicWidth = end == measured.caretXByByte.end() ? 0.0f : *end;
        heading.Font = font;
        heading.PixelSize = pixelSize;
        heading.LetterSpacing = letterSpacing;
    }
    const float baseline = Rendering::Text::TextLayout::FirstBaselineInContentBoxPx(
        label->GetLayoutHeight() * scale, font->GetFontLineMetrics(pixelSize),
        Rendering::Text::TextLayout::ResolveLineBoxPx(visual.LineHeight, visual.FontSize, scale), 1);
    underline->Style().Set(Style::PositionLeft, StyleLength::Px(label->GetLayoutX() - button->GetLayoutX()));
    underline->Style().Set(Style::PositionTop,
        StyleLength::Px(label->GetLayoutY() - button->GetLayoutY() + baseline / scale));
    underline->Style().Set(Style::Width, StyleLength::Px(heading.MnemonicWidth / scale));
}

void EditorTitleBar::OpenMenu(uint32_t menuId)
{
    // Opening a modal native menu from an input handler would retain that
    // handler's stack across reentrant native paint/resize messages.
    PostSafeAction([this, menuId]()
    {
        if (m_Toolbar)
            m_Toolbar->ShowTitleBarMenu(menuId);
    });
}

void EditorTitleBar::OnPostLayout()
{
    if (!m_Toolbar || !m_Window || !m_AssetsLoaded || m_Window->IsFullscreen())
        return;
    UIManager* ui = GetOwnerManager();
    if (!ui)
        return;
    const float scale = std::max(0.01f, ui->GetContentScale());
    const bool integratedCaption = m_Toolbar->GetPresentation() == ToolbarPresentation::WindowTitleBar;
    const auto metrics = integratedCaption ? m_Toolbar->GetTitleBarMetrics() : TitleBarMetrics{};
    UIElement* icon = m_Icon.Get();
    Label* title = m_Title.Get();
    UIElement* captionContents[] = {icon, title, m_CaptionLabels[0].Get(), m_CaptionLabels[1].Get(),
                                    m_CaptionLabels[2].Get()};
    for (UIElement* element : captionContents)
    {
        if (element)
            element->Style().Set(Style::Display, integratedCaption ? DisplayMode::Flex : DisplayMode::None);
    }
    const float captionWidth = static_cast<float>(metrics.CaptionWidth) / scale;
    const float height = std::max(kMinimumHeight, static_cast<float>(metrics.Height) / scale);
    Style().Set(Style::Height, StyleLength::Px(height));
    Style().Set(Style::PaddingRight, StyleLength::Px(captionWidth));

    float required = integratedCaption ? kMinimumDragWidth : 0.0f;
    for (auto& heading : m_MenuHeadings)
    {
        if (auto* button = heading.Control.Get())
        {
            UpdateMnemonicLayout(heading);
            required += button->WidthForText(button->GetText());
        }
    }
    if (icon)
        required += icon->GetLayoutWidth();
    m_Collapsed = required > GetLayoutWidth() - captionWidth;
    UIElement* host = m_MenusHost.Get();
    UIElement* overflow = m_Overflow.Get();
    if (host)
        host->Style().Set(Style::Display, m_Collapsed ? DisplayMode::None : DisplayMode::Flex);
    if (overflow)
        overflow->Style().Set(Style::Display, m_Collapsed ? DisplayMode::Flex : DisplayMode::None);

    if (title)
    {
        UIElement* menus = m_Collapsed ? overflow : host;
        const float menuRight = menus ? menus->GetLayoutX() + menus->GetLayoutWidth() - GetLayoutX() : 0.0f;
        const float inset = std::max(menuRight + kTitleGap, captionWidth);
        const float titleWidth = std::max(0.0f, GetLayoutWidth() - 2.0f * inset);
        title->Style().Set(Style::PositionLeft, StyleLength::Px((GetLayoutWidth() - titleWidth) * 0.5f));
        title->Style().Set(Style::Width, StyleLength::Px(titleWidth));
        // The tooltip shows the full title only when the ellipsis cuts it.
        const bool truncated = ui->MeasureTextWidth(*title, title->GetText()) > titleWidth;
        std::string tooltip = truncated ? title->GetText() : std::string();
        if (title->GetTooltip() != tooltip)
            title->SetTooltip(std::move(tooltip));
    }

    const char* glyphs[] = {kMinimizeGlyph, metrics.Maximized ? kRestoreGlyph : kMaximizeGlyph, kCloseGlyph};
    for (size_t index = 0; integratedCaption && index < metrics.Buttons.size() && index < m_CaptionLabels.size(); ++index)
    {
        if (auto* label = m_CaptionLabels[index].Get())
        {
            const auto& bounds = metrics.Buttons[index];
            label->SetText(glyphs[index]);
            if (bounds.Button == metrics.HoveredButton)
                label->AddClass("hovered");
            else
                label->RemoveClass("hovered");
            label->Style().Set(Style::PositionLeft, StyleLength::Px(static_cast<float>(bounds.Bounds.X) / scale));
            label->Style().Set(Style::PositionTop, StyleLength::Px(static_cast<float>(bounds.Bounds.Y) / scale));
            label->Style().Set(Style::Width, StyleLength::Px(static_cast<float>(bounds.Bounds.Width) / scale));
            label->Style().Set(Style::Height, StyleLength::Px(static_cast<float>(bounds.Bounds.Height) / scale));
        }
    }

    std::vector<TitleBarMenuRegion> regions;
    if (m_Collapsed && overflow)
    {
        regions.push_back({0, {Pixels(overflow->GetLayoutX(), scale), Pixels(overflow->GetLayoutY(), scale),
                               Pixels(overflow->GetLayoutWidth(), scale), Pixels(overflow->GetLayoutHeight(), scale)}});
    }
    else
    {
        for (const auto& heading : m_MenuHeadings)
        {
            if (auto* button = heading.Control.Get())
            {
                regions.push_back({heading.Id, {Pixels(button->GetLayoutX(), scale), Pixels(button->GetLayoutY(), scale),
                                               Pixels(button->GetLayoutWidth(), scale), Pixels(button->GetLayoutHeight(), scale)}});
            }
        }
    }
    PushTitleBarLayout(Pixels(height, scale), std::move(regions));
}

void EditorTitleBar::PushTitleBarLayout(int height, std::vector<TitleBarMenuRegion> regions)
{
    if (height == m_PushedHeight && regions == m_PushedRegions)
        return;
    m_PushedHeight = height;
    m_PushedRegions = regions;
    m_Toolbar->SetTitleBarLayout(height, std::move(regions));
}
} // namespace GameEngine

namespace RegisterWidgets
{
static auto s_EditorTitleBar = GameEngine::UIRegistration::RegisterWithFactory<GameEngine::EditorTitleBar>(
    "EditorTitleBar", []() { return std::make_unique<GameEngine::EditorTitleBar>(); });
}
