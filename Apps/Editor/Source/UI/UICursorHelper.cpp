#include "UI/UICursorHelper.h"

#include "Core/WindowInputRouter.h"
#include "PlayMode/WindowCursorLock.h"
#include "UI/UIStyle.h"

namespace GameEngine
{
namespace
{

// A UI value drag locks the cursor with the motion an unlocked drag sees, so
// the drag does not change speed when the lock begins.
void BeginUIPointerLock(Editor::WindowCursorLock* cursorLock)
{
    cursorLock->LockForUiDrag();
}

void EndUIPointerLock(UIManager* ui, Platform::Window* window, Editor::WindowCursorLock* cursorLock)
{
    cursorLock->UnlockForUiDrag();
    // The game can still hold the lock; then the cursor stays hidden.
    if (cursorLock->IsLocked())
        return;
    // The cursor shows again where the lock began, which no move reports.
    float clientX = 0.0f;
    float clientY = 0.0f;
    window->GetCursorClientPosition(clientX, clientY);
    WindowInputRouter::SendUiMouseFromClientPixels(window, ui, clientX, clientY);
}

} // namespace

void SetupUICursorCallback(UIManager* ui, Platform::Window* window, Editor::WindowCursorLock* cursorLock)
{
    if (!ui || !window || !cursorLock)
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

    ui->SetPointerLockCallback([ui, window, cursorLock](bool locked)
    {
        if (locked)
            BeginUIPointerLock(cursorLock);
        else
            EndUIPointerLock(ui, window, cursorLock);
    });
}

} // namespace GameEngine
