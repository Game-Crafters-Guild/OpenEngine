#pragma once

#include <cstdint>

namespace GameEngine::Editor
{

/// The phase timings of the window that rendered the Scene View in one editor frame, plus the
/// Application phases the editor copies beside them. Milliseconds.
struct SceneViewFrameTimings
{
    double pollMs = 0.0;
    double inputMs = 0.0;
    double appUpdateMs = 0.0;
    double engineUpdateMs = 0.0;
    double beginFrameMs = 0.0;
    double worldViewsMs = 0.0;     // RecordWindowWorldViews (entity / scene rendering record)
    double thumbnailsMs = 0.0;
    double uiRecordMs = 0.0;
    double terminalPassesMs = 0.0; // BuildWindowTerminalPasses + debug callbacks
    double rgCompileMs = 0.0;
    double rgExecuteMs = 0.0;
    // First-use SPIR-V variant compile time. Front-loaded by
    // RenderServices::PrewarmActiveVariants before rg->Compile so it no
    // longer inflates rgExecuteMs; reported here so the spike is still
    // visible in render_stats.
    double variantCompileMs = 0.0;
    double prewarmMs = 0.0;
    double presentMs = 0.0;
    // Application::GetFrameCount() of the frame these were measured in.
    std::uint64_t frameIndex = 0;
};

/// The sub-phases of the editor's AppUpdate in one frame. They sum to roughly the outer
/// AppUpdateMs minus small untimed fillers. Milliseconds.
struct AppUpdateSubPhaseTimings
{
    double preUiMs = 0.0;       // startup + modals + polls + base Update + ECS ProcessCommands + SceneEditor + flushes + pre-UI deferred
    double debugPanelsMs = 0.0; // CollectEditorDebugMetrics + PanelManager->UpdatePanels (log flush + debug panels)
    double uiWindowsMs = 0.0;   // UI UpdateAllWindows loop (per-window UIManager::Update)
    double tailMs = 0.0;        // post-UI deferred actions + TickPlayFullscreenFocus + input routing + hotkeys
};

} // namespace GameEngine::Editor
