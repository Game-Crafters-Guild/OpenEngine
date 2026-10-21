#include "EditorInputActions.h"

#include "Editor/Shortcuts/EditorShortcuts.h"

namespace GameEngine
{
namespace EditorInput
{

namespace
{
// A key with exactly `mods` held: Shift+F3 is not F3.
void RegisterExactChordAction(Input::InputSystem& input, Input::ActionId id, int key, int mods)
{
    Input::ActionBinding binding{Input::DeviceType::Keyboard, key, 1.0f};
    binding.requiredMods = static_cast<Input::ModifierFlags>(mods);
    binding.forbiddenMods = Input::kShortcutModifiers & ~binding.requiredMods;
    input.RegisterAction(kEditorGlobalContext, {id, {binding}, /*isAxis=*/false, /*hostReserved=*/false});
}
} // namespace

void RegisterEditorInputActions(Input::InputSystem& input)
{
    // Scene View movement (WASD + Q/E world vertical while RMB look is active).
    Input::ActionDesc moveForward{};
    moveForward.id = kSceneViewMoveForward;
    moveForward.isAxis = false;
    moveForward.bindings.push_back({Input::DeviceType::Keyboard, Input::kKeyCode_W, 1.0f});
    input.RegisterAction(kSceneViewContext, moveForward);

    Input::ActionDesc moveBackward{};
    moveBackward.id = kSceneViewMoveBackward;
    moveBackward.isAxis = false;
    moveBackward.bindings.push_back({Input::DeviceType::Keyboard, Input::kKeyCode_S, 1.0f});
    input.RegisterAction(kSceneViewContext, moveBackward);

    Input::ActionDesc moveLeft{};
    moveLeft.id = kSceneViewMoveLeft;
    moveLeft.isAxis = false;
    moveLeft.bindings.push_back({Input::DeviceType::Keyboard, Input::kKeyCode_A, 1.0f});
    input.RegisterAction(kSceneViewContext, moveLeft);

    Input::ActionDesc moveRight{};
    moveRight.id = kSceneViewMoveRight;
    moveRight.isAxis = false;
    moveRight.bindings.push_back({Input::DeviceType::Keyboard, Input::kKeyCode_D, 1.0f});
    input.RegisterAction(kSceneViewContext, moveRight);

    // World vertical while RMB look is held. The same keys switch QWER tools
    // when look is idle; SceneViewPanel suppresses those hotkeys during look.
    Input::ActionDesc moveUp{};
    moveUp.id = kSceneViewMoveUp;
    moveUp.isAxis = false;
    moveUp.bindings.push_back({Input::DeviceType::Keyboard, Input::kKeyCode_E, 1.0f});
    input.RegisterAction(kSceneViewContext, moveUp);

    Input::ActionDesc moveDown{};
    moveDown.id = kSceneViewMoveDown;
    moveDown.isAxis = false;
    moveDown.bindings.push_back({Input::DeviceType::Keyboard, Input::kKeyCode_Q, 1.0f});
    input.RegisterAction(kSceneViewContext, moveDown);

    Input::ActionDesc moveFaster{};
    moveFaster.id = kSceneViewMoveFaster;
    moveFaster.isAxis = false;
    moveFaster.bindings.push_back({Input::DeviceType::Keyboard, Input::kKeyCode_LeftShift, 1.0f});
    moveFaster.bindings.push_back({Input::DeviceType::Keyboard, Input::kKeyCode_RightShift, 1.0f});
    input.RegisterAction(kSceneViewContext, moveFaster);

    // Scene View look axes (mouse delta X/Y mapped to yaw/pitch degrees per pixel).
    Input::ActionDesc lookYaw{};
    lookYaw.id = kSceneViewLookYaw;
    lookYaw.isAxis = true;
    lookYaw.bindings.push_back({Input::DeviceType::Mouse, Input::kMouseAxisX, 1.0f});
    input.RegisterAction(kSceneViewContext, lookYaw);

    Input::ActionDesc lookPitch{};
    lookPitch.id = kSceneViewLookPitch;
    lookPitch.isAxis = true;
    lookPitch.bindings.push_back({Input::DeviceType::Mouse, Input::kMouseAxisY, 1.0f});
    input.RegisterAction(kSceneViewContext, lookPitch);

    // Global editor actions (currently routed to the primary Scene View).
    //
    // hostReserved: the editor's modifier chords (Alt+F Frame All, Ctrl/Cmd+F
    // fullscreen, Ctrl/Cmd+P play, Ctrl/Cmd+S save and save-as, universal
    // search) reach the editor ahead of a game running in the Game View. The
    // bare-key actions (G gizmos, F frame selection, Escape discard review)
    // are not reserved: games bind bare keys, so those stay the game's first.
    Input::ActionDesc toggleGizmos{};
    toggleGizmos.id = kEditorToggleGizmos;
    toggleGizmos.isAxis = false;
    toggleGizmos.bindings.push_back({Input::DeviceType::Keyboard, Input::kKeyCode_G, 1.0f});
    input.RegisterAction(kEditorGlobalContext, toggleGizmos);

    Input::ActionDesc frameSelection{};
    frameSelection.id = kEditorFrameSelection;
    frameSelection.isAxis = false;
    {
        Input::ActionBinding b{Input::DeviceType::Keyboard, Input::kKeyCode_F, 1.0f};
        b.forbiddenMods = Input::kShortcutModifiers;
        frameSelection.bindings.push_back(b);
    }
    input.RegisterAction(kEditorGlobalContext, frameSelection);

    Input::ActionDesc frameAll{};
    frameAll.id = kEditorFrameAll;
    frameAll.isAxis = false;
    frameAll.hostReserved = true;
    {
        // Alt+F: frame all. Moved off Shift+F to avoid conflict with the
        // Assets panel's Shift+F preview toggle.
        Input::ActionBinding b{Input::DeviceType::Keyboard, Input::kKeyCode_F, 1.0f};
        b.requiredMods = Input::ModifierFlags::Alt;
        frameAll.bindings.push_back(b);
    }
    input.RegisterAction(kEditorGlobalContext, frameAll);

    // Cmd/Ctrl+F: toggle editor window fullscreen (borderless) without entering Play Mode.
    Input::ActionDesc toggleFullscreen{};
    toggleFullscreen.id = kEditorToggleWindowFullscreen;
    toggleFullscreen.isAxis = false;
    toggleFullscreen.hostReserved = true;
    {
        Input::ActionBinding b{Input::DeviceType::Keyboard, Input::kKeyCode_F, 1.0f};
        b.requiredMods = Input::ModifierFlags::Super;
        toggleFullscreen.bindings.push_back(b);
    }
    {
        Input::ActionBinding b{Input::DeviceType::Keyboard, Input::kKeyCode_F, 1.0f};
        b.requiredMods = Input::ModifierFlags::Control;
        toggleFullscreen.bindings.push_back(b);
    }
    input.RegisterAction(kEditorGlobalContext, toggleFullscreen);

    // Cmd/Ctrl+P: toggle play mode (enter play when in Edit, stop when playing/paused).
    Input::ActionDesc togglePlayMode{};
    togglePlayMode.id = kEditorTogglePlayMode;
    togglePlayMode.isAxis = false;
    togglePlayMode.hostReserved = true;
    {
        Input::ActionBinding b{Input::DeviceType::Keyboard, Input::kKeyCode_P, 1.0f};
        b.requiredMods = Input::ModifierFlags::Super;
        togglePlayMode.bindings.push_back(b);
    }
    {
        Input::ActionBinding b{Input::DeviceType::Keyboard, Input::kKeyCode_P, 1.0f};
        b.requiredMods = Input::ModifierFlags::Control;
        togglePlayMode.bindings.push_back(b);
    }
    input.RegisterAction(kEditorGlobalContext, togglePlayMode);

    // Escape: discard Change Review. While the Game View runtime sink is live it
    // answers first, so this sees Escape only when the game does not claim it.
    Input::ActionDesc discardPlayReview{};
    discardPlayReview.id = kEditorDiscardPlayReview;
    discardPlayReview.isAxis = false;
    discardPlayReview.bindings.push_back({Input::DeviceType::Keyboard, Input::kKeyCode_Escape, 1.0f});
    input.RegisterAction(kEditorGlobalContext, discardPlayReview);

    // Ctrl+S / Cmd+S: save current scene (Save or Save As).
    Input::ActionDesc saveScene{};
    saveScene.id = kEditorSaveScene;
    saveScene.isAxis = false;
    saveScene.hostReserved = true;
    {
        Input::ActionBinding b{Input::DeviceType::Keyboard, Input::kKeyCode_S, 1.0f};
        b.requiredMods = Input::ModifierFlags::Control;
        b.forbiddenMods = Input::ModifierFlags::Shift;
        saveScene.bindings.push_back(b);
    }
    {
        Input::ActionBinding b{Input::DeviceType::Keyboard, Input::kKeyCode_S, 1.0f};
        b.requiredMods = Input::ModifierFlags::Super;
        b.forbiddenMods = Input::ModifierFlags::Shift;
        saveScene.bindings.push_back(b);
    }
    input.RegisterAction(kEditorGlobalContext, saveScene);

    // Ctrl+Shift+S / Cmd+Shift+S: always open the Save As dialog.
    Input::ActionDesc saveSceneAs{};
    saveSceneAs.id = kEditorSaveSceneAs;
    saveSceneAs.isAxis = false;
    saveSceneAs.hostReserved = true;
    {
        Input::ActionBinding b{Input::DeviceType::Keyboard, Input::kKeyCode_S, 1.0f};
        b.requiredMods = Input::ModifierFlags::Control | Input::ModifierFlags::Shift;
        saveSceneAs.bindings.push_back(b);
    }
    {
        Input::ActionBinding b{Input::DeviceType::Keyboard, Input::kKeyCode_S, 1.0f};
        b.requiredMods = Input::ModifierFlags::Super | Input::ModifierFlags::Shift;
        saveSceneAs.bindings.push_back(b);
    }
    input.RegisterAction(kEditorGlobalContext, saveSceneAs);

    RegisterUniversalSearchShortcut(input);

    // Diagnostics: bare function keys, not reserved, so a game in Play that binds
    // or polls one takes it first (Editor::DiagnosticShortcuts acts on them).
#if GE_ENABLE_CPU_PROFILING
    RegisterExactChordAction(input, kEditorCpuProfiler, Input::kKeyCode_F2, 0);
    RegisterExactChordAction(input, kEditorCpuProfilerOff, Input::kKeyCode_F2, Input::kModShift);
#endif
    RegisterExactChordAction(input, kEditorShowUIDemo, Input::kKeyCode_F3, 0);

    // Apply any user shortcut overrides persisted in Preferences.json on
    // top of the defaults just registered above.
    Editor::ApplyShortcutOverridesFromPreferences(input);

    // Push contexts (Scene View first, then global so it has higher priority).
    input.PushContext(kSceneViewContext);
    input.PushContext(kEditorGlobalContext);

    // Game View context is opt-in: enabled only while playing and the Game View is focused.
    input.PushContext(kGameViewContext);
    input.SetContextEnabled(kGameViewContext, false);
}

} // namespace EditorInput
} // namespace GameEngine
