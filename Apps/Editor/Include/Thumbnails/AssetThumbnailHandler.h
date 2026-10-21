#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace GameEngine {

class UIManager;
namespace Rendering::RenderGraph { class RGFrame; }

// Base interface for per-asset-type thumbnail generators used by
// ThumbnailService. Each handler is responsible for a single asset
// type and may call into engine subsystems as needed.
class AssetThumbnailHandler {
public:
    virtual ~AssetThumbnailHandler() = default;

    // Thumbnail handlers need to know the Assets/ root so they can
    // return relative paths that the UI layer can use directly in
    // CSS-style background-image URLs.
    void SetAssetsRoot(const std::filesystem::path& root) { m_AssetsRoot = root; }

    // Persistent thumbnail cache root (<ProjectRoot>/.Editor/Thumbnails).
    // ThumbnailService pushes it to every handler on registration and again on
    // each project switch. Handlers that keep generated images on disk override
    // this; the default ignores it. An empty root means no project is open —
    // handlers must not write anywhere else in that case.
    virtual void SetCacheRoot(const std::filesystem::path& root) { (void)root; }

    // Drop transient state keyed to the project being closed. Asset GUIDs and
    // paths do not survive a project switch, so anything a handler remembers
    // about them — queued work, cached decisions, spawned preview state — has
    // to go before the next project's requests arrive. Handlers that hold
    // nothing across a switch ignore it.
    virtual void ResetForProjectSwitch() {}

    // Schedules thumbnail generation or retrieval for a specific asset.
    // Returns a relative path (from Assets/) if a thumbnail is already
    // available; otherwise may return empty and invoke onReady later.
    virtual std::string GetOrRequest(const std::filesystem::path& assetPath,
                                     int desiredSize,
                                     std::function<void(const std::string& relPath)> onReady,
                                     bool StaticModelListThumbnail = false) = 0;

    // Handlers that render on the GPU: ThumbnailService calls these for every registered handler,
    // each window frame. Declare this window's pending renders into its frame.
    virtual void TickRenderRG(uint64_t windowId, Rendering::RenderGraph::RGFrame& frame)
    {
        (void)windowId;
        (void)frame;
    }
    // Bind (or publish, on a pure RenderGraph frame) the window's rendered thumbnails to its UI.
    // Returns true when a binding was added or removed.
    virtual bool RegisterReadyThumbnailsRG(uint64_t windowId, UIManager* ui,
                                           Rendering::RenderGraph::RGFrame* pureFrame)
    {
        (void)windowId;
        (void)ui;
        (void)pureFrame;
        return false;
    }
    // The UI asked for an engine texture name nothing has rendered in this window yet. A handler
    // that recognises the name queues it; others ignore it.
    virtual void EnsureEngineThumbnailRequested(uint64_t windowId, const std::string& engineName)
    {
        (void)windowId;
        (void)engineName;
    }
    virtual bool IsEngineThumbnailReadyForWindow(uint64_t windowId, const std::string& engineName) const
    {
        (void)windowId;
        (void)engineName;
        return false;
    }

protected:
    std::filesystem::path m_AssetsRoot;
};

} // namespace GameEngine

