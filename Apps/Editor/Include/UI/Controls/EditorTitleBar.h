#pragma once

#include "UI/UIElement.h"
#include "Platform/Toolbar.h"

#include <cstdint>
#include <vector>

namespace GameEngine
{
class Button;
namespace Platform { class Window; }
namespace Rendering::Text { class FontAtlas; }

/// Editor-owned caption contents. Native menus and frame behavior stay in Platform.
class EditorTitleBar final : public UIElement
{
  public:
    EditorTitleBar();
    void Bind(INativeToolbar* toolbar, Platform::Window* window);
    void UpdateState();
    void OnPostLayout() override;
    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style,
                              float x, float y, float width, float height) override;

  private:
    struct MenuHeading
    {
        WeakRef<Button> Control;
        uint32_t Id = 0;
        Rendering::Text::FontAtlas* Font = nullptr;
        float PixelSize = 0.0f;
        float LetterSpacing = 0.0f;
        float MnemonicWidth = 0.0f;
    };

    bool LoadStyle();
    void RebuildMenus();
    void UpdateMnemonicLayout(MenuHeading& heading);
    void OpenMenu(uint32_t menuId);

    INativeToolbar* m_Toolbar = nullptr;
    Platform::Window* m_Window = nullptr;
    uint64_t m_MenuRevision = UINT64_MAX;
    bool m_StyleLoaded = false;
    bool m_Collapsed = false;
    TitleBarMetrics m_CaptionMetrics;
    std::vector<MenuHeading> m_MenuHeadings;
};
} // namespace GameEngine
