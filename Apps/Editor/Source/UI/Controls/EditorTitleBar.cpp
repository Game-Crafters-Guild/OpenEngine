#include "UI/Controls/EditorTitleBar.h"

#include "AssetCore/AssetTypes.h"
#include "Assets/AssetManager.h"
#include "Core/Engine.h"
#include "Logger/Logger.h"
#include "Platform/Toolbar.h"
#include "Platform/Window.h"
#include "Rendering/Text/FontAtlas.h"
#include "Rendering/Text/TextLayout.h"
#include "UI/Assets/UIStyleAsset.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/ElementBuilder.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/StyleProperties.h"
#include "UI/UIManager.h"
#include "UI/UIPrimitive.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace GameEngine
{
using UI::operator""_ui;

namespace
{
// A caption must remain grabbable even when menu contributions fill the row.
constexpr float kMinimumDragWidth = 64.0f;
constexpr float kMinimumHeight = 32.0f;

int Pixels(float logical, float scale)
{
    return static_cast<int>(std::lround(logical * scale));
}
} // namespace

EditorTitleBar::EditorTitleBar()
{
    SetId("EditorTitleBar");
    AddClass("editor-title-bar");
    // Also hides the control before its stylesheet is loaded and on platforms
    // whose menus live outside the window (including macOS).
    Overrides().Set(Style::Display, DisplayMode::None);

    AddChild(UI::Element<UIElement>()
        .Id("title-bar-icon"_ui)
        .Class("title-bar-icon"_ui)
        .Build());
    AddChild(UI::Element<UIElement>()
        .Id("title-bar-menus"_ui)
        .Class("title-bar-menus"_ui)
        .Build());
    AddChild(UI::Element<Button>()
        .Id("title-bar-overflow"_ui)
        .Class("title-bar-menu"_ui, "title-bar-overflow"_ui)
        .Set(&Button::SetText, "Menus")
        .Set(&Button::SetOnClick, [this](UIEvent&) { OpenMenu(0); })
        .Build());
    AddChild(UI::Element<Label>()
        .Id("title-bar-title"_ui)
        .Class("title-bar-title"_ui)
        .Build());
    AddChild(UI::Element<Label>()
        .Id("title-bar-minimize"_ui)
        .Class("title-bar-caption-icon"_ui)
        .Build());
    AddChild(UI::Element<Label>()
        .Id("title-bar-maximize"_ui)
        .Class("title-bar-caption-icon"_ui)
        .Build());
    AddChild(UI::Element<Label>()
        .Id("title-bar-close"_ui)
        .Class("title-bar-caption-icon"_ui)
        .Build());
}

bool EditorTitleBar::LoadStyle()
{
    if (m_StyleLoaded)
        return true;
    UIManager* ui = GetOwnerManager();
    if (!ui)
        return false;
    auto& assets = EngineCore::GetInstance().GetAssetManager();
    const GUID styleGuid = assets.ResolveAssetGuid("UI/controls/EditorTitleBar.css", "Editor");
    if (styleGuid.IsNull())
        return false;
    auto style = assets.LoadAssetAsync(styleGuid, AssetLoadPriority::High).get();
    if (!style || style->GetType() != AssetType::UIStyle ||
        !ui->AttachStyleToSubtreeFromAsset(this, *static_cast<UIStyleAsset*>(style.get())))
        return false;
    m_StyleLoaded = true;
    return true;
}

void EditorTitleBar::Bind(INativeToolbar* toolbar, Platform::Window* window)
{
    m_Toolbar = toolbar;
    m_Window = window;
    if (!toolbar || !window || toolbar->GetPresentation() == ToolbarPresentation::NativeMenuBar)
    {
        Overrides().Set(Style::Display, DisplayMode::None);
        return;
    }
    if (!LoadStyle())
    {
        Logger::Log::Error("EditorTitleBar: failed to load title bar stylesheet; native frame retained");
        m_Toolbar = nullptr;
        m_Window = nullptr;
        return;
    }
    UpdateState();
}

void EditorTitleBar::UpdateState()
{
    if (!m_StyleLoaded || !m_Toolbar || !m_Window ||
        m_Toolbar->GetPresentation() == ToolbarPresentation::NativeMenuBar)
        return;
    const bool fullscreen = m_Window->IsFullscreen();
    Overrides().Set(Style::Display, fullscreen ? DisplayMode::None : DisplayMode::Flex);
    if (fullscreen)
    {
        m_Toolbar->SetTitleBarLayout(0, {});
        return;
    }
    if (auto* title = dynamic_cast<Label*>(FindById("title-bar-title"_sid)))
    {
        if (title->GetText() != m_Window->GetTitle())
        {
            title->SetText(m_Window->GetTitle());
            title->SetTooltip(m_Window->GetTitle());
        }
    }
    if (m_MenuRevision != m_Toolbar->GetMenuRevision() && !m_Toolbar->IsMenuOpen())
        RebuildMenus();
    // Update even without a layout solve: DPI and native caption metrics can
    // change while the CSS geometry and the scene title stay the same.
    OnPostLayout();
}

void EditorTitleBar::RebuildMenus()
{
    UIElement* host = FindById("title-bar-menus"_sid);
    if (!host)
        return;
    host->RemoveAllChildren();
    m_MenuHeadings.clear();
    for (const auto& menu : m_Toolbar->GetMenus())
    {
        auto button = UI::Element<Button>()
            .Id("title-bar-menu-" + std::to_string(menu.Id))
            .Class("title-bar-menu"_ui)
            .Set(&Button::SetText, menu.Title, &Button::SetTooltip, menu.Title)
            .Set(&Button::SetOnClick, [this, id = menu.Id](UIEvent&) { OpenMenu(id); })
            .Children(UI::Element<UIElement>().Class("title-bar-mnemonic"_ui))
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
    underline->Overrides().Set(Style::PositionLeft, StyleLength::Px(label->GetLayoutX() - button->GetLayoutX()));
    underline->Overrides().Set(Style::PositionTop,
        StyleLength::Px(label->GetLayoutY() - button->GetLayoutY() + baseline / scale));
    underline->Overrides().Set(Style::Width, StyleLength::Px(heading.MnemonicWidth / scale));
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
    if (!m_Toolbar || !m_Window || !m_StyleLoaded || m_Window->IsFullscreen())
        return;
    UIManager* ui = GetOwnerManager();
    if (!ui)
        return;
    const float scale = std::max(0.01f, ui->GetContentScale());
    const bool integratedCaption = m_Toolbar->GetPresentation() == ToolbarPresentation::WindowTitleBar;
    const auto metrics = integratedCaption ? m_Toolbar->GetTitleBarMetrics() : TitleBarMetrics{};
    constexpr StringId kCaptionContents[] = {
        "title-bar-icon"_sid, "title-bar-title"_sid, "title-bar-minimize"_sid,
        "title-bar-maximize"_sid, "title-bar-close"_sid};
    for (const auto id : kCaptionContents)
    {
        if (auto* element = FindById(id))
            element->Overrides().Set(Style::Display, integratedCaption ? DisplayMode::Flex : DisplayMode::None);
    }
    const float captionWidth = static_cast<float>(metrics.CaptionWidth) / scale;
    const float height = std::max(kMinimumHeight, static_cast<float>(metrics.Height) / scale);
    Overrides().Set(Style::Height, StyleLength::Px(height));
    Overrides().Set(Style::PaddingRight, StyleLength::Px(captionWidth));

    float required = integratedCaption ? kMinimumDragWidth : 0.0f;
    for (auto& heading : m_MenuHeadings)
    {
        if (auto* button = heading.Control.Get())
        {
            UpdateMnemonicLayout(heading);
            required += button->WidthForText(button->GetText());
        }
    }
    UIElement* icon = FindById("title-bar-icon"_sid);
    if (icon)
        required += icon->GetLayoutWidth();
    m_Collapsed = required > GetLayoutWidth() - captionWidth;
    auto* host = FindById("title-bar-menus"_sid);
    auto* overflow = FindById("title-bar-overflow"_sid);
    if (host)
        host->Overrides().Set(Style::Display, m_Collapsed ? DisplayMode::None : DisplayMode::Flex);
    if (overflow)
        overflow->Overrides().Set(Style::Display, m_Collapsed ? DisplayMode::Flex : DisplayMode::None);

    if (auto* title = FindById("title-bar-title"_sid))
    {
        UIElement* menus = m_Collapsed ? overflow : host;
        const float menuRight = menus ? menus->GetLayoutX() + menus->GetLayoutWidth() - GetLayoutX() : 0.0f;
        const float inset = std::max(menuRight + 12.0f, captionWidth);
        const float titleWidth = std::max(0.0f, GetLayoutWidth() - 2.0f * inset);
        title->Overrides().Set(Style::PositionLeft, StyleLength::Px((GetLayoutWidth() - titleWidth) * 0.5f));
        title->Overrides().Set(Style::Width, StyleLength::Px(titleWidth));
    }

    if (m_CaptionMetrics != metrics)
    {
        m_CaptionMetrics = metrics;
        MarkContentDirty();
    }
    // Windows' ChromeMinimize, ChromeMaximize/ChromeRestore and ChromeClose
    // glyphs come from the system icon font, including its rounded outlines.
    constexpr StringId kCaptionIds[] = {"title-bar-minimize"_sid, "title-bar-maximize"_sid, "title-bar-close"_sid};
    const char* glyphs[] = {"\xEE\xA4\xA1", metrics.Maximized ? "\xEE\xA4\xA3" : "\xEE\xA4\xA2", "\xEE\xA2\xBB"};
    for (size_t index = 0; integratedCaption && index < metrics.Buttons.size(); ++index)
    {
        if (auto* label = dynamic_cast<Label*>(FindById(kCaptionIds[index])))
        {
            const auto& bounds = metrics.Buttons[index];
            label->SetText(glyphs[index]);
            label->Overrides().Set(Style::PositionLeft, StyleLength::Px(static_cast<float>(bounds.X) / scale));
            label->Overrides().Set(Style::PositionTop, StyleLength::Px(static_cast<float>(bounds.Y) / scale));
            label->Overrides().Set(Style::Width, StyleLength::Px(static_cast<float>(bounds.Width) / scale));
            label->Overrides().Set(Style::Height, StyleLength::Px(static_cast<float>(bounds.Height) / scale));
        }
    }

    std::vector<TitleBarMenuRegion> regions;
    if (m_Collapsed && overflow)
    {
        regions.push_back({0, Pixels(overflow->GetLayoutX(), scale), Pixels(overflow->GetLayoutY(), scale),
                           Pixels(overflow->GetLayoutWidth(), scale), Pixels(overflow->GetLayoutHeight(), scale)});
    }
    else
    {
        for (const auto& heading : m_MenuHeadings)
        {
            if (auto* button = heading.Control.Get())
            {
                regions.push_back({heading.Id, Pixels(button->GetLayoutX(), scale), Pixels(button->GetLayoutY(), scale),
                                   Pixels(button->GetLayoutWidth(), scale), Pixels(button->GetLayoutHeight(), scale)});
            }
        }
    }
    m_Toolbar->SetTitleBarLayout(Pixels(height, scale), std::move(regions));
}

void EditorTitleBar::OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& /*style*/,
                                         float x, float y, float /*width*/, float /*height*/)
{
    if (ctx.OffThread)
    {
        if (ctx.EscalateFlag)
            *ctx.EscalateFlag = true;
        return;
    }
    for (const auto& button : m_CaptionMetrics.Buttons)
    {
        if (button.Width <= 0 || button.Height <= 0)
            continue;
        const float left = x + static_cast<float>(button.X);
        const float top = y + static_cast<float>(button.Y);
        const float width = static_cast<float>(button.Width);
        const float height = static_cast<float>(button.Height);
        const bool hovered = button.Button == m_CaptionMetrics.HoveredButton;
        if (hovered)
        {
            const uint32_t fill = button.Button == TitleBarButton::Close ? UI::PackColor(0.77f, 0.17f, 0.11f, 1.0f) :
                                                                        UI::PackColor(1.0f, 1.0f, 1.0f, 0.12f);
            ctx.Emit(UI::MakeRect(left, top, width, height, fill));
        }
    }
}
} // namespace GameEngine

namespace RegisterWidgets
{
static auto s_EditorTitleBar = GameEngine::UIRegistration::RegisterWithFactory<GameEngine::EditorTitleBar>(
    "EditorTitleBar", []() { return std::make_unique<GameEngine::EditorTitleBar>(); });
}
