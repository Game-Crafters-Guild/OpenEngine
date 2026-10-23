#include "UI/UICursorHelper.h"

#include "Core/WindowInputRouter.h"
#include "UI/UIStyle.h"

namespace GameEngine
{
namespace
{

// A UI value drag locks the cursor with the motion an unlocked drag sees, so
// the drag does not change speed when the lock begins.
void ApplyUIPointerLock(UIManager* ui, Platform::Window* window, bool locked)
{
    if (locked)
    {
        window->LockCursor(Platform::Window::LockedCursorMotion::Cursor);
        return;
    }
    window->UnlockCursor();
    // The cursor shows again where the lock began, which no move reports.
    float clientX = 0.0f;
    float clientY = 0.0f;
    window->GetCursorClientPosition(clientX, clientY);
    WindowInputRouter::SendUiMouseFromClientPixels(window, ui, clientX, clientY);
}

} // namespace

void SetupUICursorCallback(UIManager* ui, Platform::Window* window)
{
    if (!ui || !window)
        return;

    ui->SetCursorCallback([window](CursorStyle cursor)
    {
        Platform::Window::CursorType winCursor = Platform::Window::CursorType::Arrow;
        switch (cursor)
        {
        case CursorStyle::Pointer:
            winCursor = Platform::Window::CursorType::Hand;
            break;
        case CursorStyle::Grab:
            winCursor = Platform::Window::CursorType::Grab;
            break;
        case CursorStyle::Grabbing:
            winCursor = Platform::Window::CursorType::Grabbing;
            break;
        case CursorStyle::Text:
            winCursor = Platform::Window::CursorType::IBeam;
            break;
        case CursorStyle::Crosshair:
            winCursor = Platform::Window::CursorType::Crosshair;
            break;
        case CursorStyle::ColResize:
            winCursor = Platform::Window::CursorType::HResize;
            break;
        case CursorStyle::RowResize:
            winCursor = Platform::Window::CursorType::VResize;
            break;
        case CursorStyle::NorthwestSoutheastResize:
            winCursor = Platform::Window::CursorType::NorthwestSoutheastResize;
            break;
        case CursorStyle::Move:
            winCursor = Platform::Window::CursorType::Move;
            break;
        case CursorStyle::NotAllowed:
            winCursor = Platform::Window::CursorType::NotAllowed;
            break;
        case CursorStyle::Auto:
        default:
            winCursor = Platform::Window::CursorType::Arrow;
            break;
        }
        window->SetCursor(winCursor);
    });

    ui->SetPointerLockCallback([ui, window](bool locked) { ApplyUIPointerLock(ui, window, locked); });
}

} // namespace GameEngine
