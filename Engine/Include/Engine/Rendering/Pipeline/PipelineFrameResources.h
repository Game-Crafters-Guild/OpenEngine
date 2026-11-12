#pragma once

#include "Rendering/Camera/CameraTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGFullscreen.h"
#include "Types/StringId.h"
#include "Types/Types.h"

#include <cstdint>
#include <string_view>

namespace GameEngine::Engine::Renderer::Pipeline
{

// Per-view frame-local target VALUES for the RenderGraph path. ViewDesc.targets
// carries old-graph logical ids; the RenderGraph spine value-passes instead (the
// slice-3 contract: effective-target resolution belongs to the caller).
struct ViewTargetsRG
{
    ::GameEngine::Rendering::ViewId View = 0;
    ::GameEngine::Rendering::RenderGraph::RGTexture Color{};
    ::GameEngine::Rendering::RenderGraph::RGTexture Depth{};
    ::GameEngine::Rendering::RenderGraph::RGTexture Resolve{}; // invalid = none
};

// A pipeline buffer the way the declaration-time binding table consumes it:
// a physical {buffer, offset, size}. Graph is valid only for device-local
// pipeline buffers — those need declared Read/Write edges; upload-ring
// allocs are host-coherent and get none.
struct PipelineBufferBindingRG
{
    ::GameEngine::Rendering::BufferHandle Buffer{};
    uint64_t Offset = 0;
    uint64_t Size = 0;
    ::GameEngine::Rendering::RenderGraph::RGBuffer Graph{};
    bool IsValid() const { return Buffer.IsValid(); }
    /// This range as a fullscreen pass's buffer input, with Graph as the buffer the pass declares a read of.
    /// The caller sets Binding and IsStorage.
    ::GameEngine::Rendering::RenderGraph::RGFullscreenBufferInput AsFullscreenInput() const
    {
        ::GameEngine::Rendering::RenderGraph::RGFullscreenBufferInput input{};
        input.Buffer = Buffer;
        input.Offset = Offset;
        input.Size = Size;
        input.Declare = Graph;
        return input;
    }
};

// The pipeline's per-frame blackboard. BeginFrame empties the tables at the start of every Declare
// and keeps their storage. Values are RGFrame-local; Frame and FrameIndex are the identity guard
// (as RenderServices::ViewFrameRG): an entry from a dead frame reads as absent.
struct PipelineFrameResources
{
    // A resource name in a view (ViewId 0 = frame scope). The key holds the name's StringId, not the
    // name, so a key copies no string and keeps no reference to the caller's string.
    struct Key
    {
        ::GameEngine::Rendering::ViewId ViewId = 0;
        StringId NameId = 0;
        Key(::GameEngine::Rendering::ViewId viewId, std::string_view name)
            : ViewId(viewId), NameId(HashStringId(name))
        {
        }
        bool operator==(const Key&) const = default;
    };
    struct KeyHash
    {
        uint64_t operator()(const Key& key) const noexcept
        {
            return key.NameId ^ (static_cast<uint64_t>(key.ViewId) * 0x9e3779b97f4a7c15ull);
        }
    };
    // Per-view scalars the pre-pass derives once per frame.
    struct ViewInfo
    {
        bool ResolveFromPipeline = false;
        // Internal (raster) extent — equals Output* except under an active
        // internal-resolution split.
        uint32_t RenderWidth = 0;
        uint32_t RenderHeight = 0;
        // Display extent (the caller's targets). Post-upscale resources size
        // against this ("extent.basis": "output" in the blueprint).
        uint32_t OutputWidth = 0;
        uint32_t OutputHeight = 0;
    };

    ::GameEngine::Rendering::RenderGraph::RGFrame* Frame = nullptr;
    // The RGFrame index of the incarnation Declare ran against. An RGFrame address is used again in
    // later frames, so a pointer match alone can accept a dead frame. FrameResourcesFor compares Frame
    // and FrameIndex.
    uint64_t FrameIndex = 0;
    FastHashMap<Key, ::GameEngine::Rendering::RenderGraph::RGTexture, KeyHash> Textures;
    FastHashMap<Key, PipelineBufferBindingRG, KeyHash> Buffers;
    FastHashMap<::GameEngine::Rendering::ViewId, ViewInfo> Views;
    // Textures holding scene colour stored at a view's pre-exposure P (pre_exposure_snapshot.comp),
    // by render graph resource id: the view's colour target, the blueprint's "preExposed" textures
    // when they materialize, and the ones nodes create (ViewDeclare::MarkPreExposed).
    FastHashMap<uint32_t, ::GameEngine::Rendering::ViewId> PreExposedTextures;

    // Empties every table for the frame Declare runs against. The tables keep their storage.
    void BeginFrame(::GameEngine::Rendering::RenderGraph::RGFrame& frame)
    {
        Frame = &frame;
        FrameIndex = frame.FrameIndex();
        Textures.clear();
        Buffers.clear();
        Views.clear();
        PreExposedTextures.clear();
    }

    // Calls fn(nameId, binding) for the frame-scope entries, then for the view's entries: a per-view
    // name shadows a frame-scope one at the binding table's last-add-wins upsert.
    template <class Fn>
    void ForEachBufferBinding(::GameEngine::Rendering::ViewId viewId, Fn&& fn) const
    {
        if (viewId != 0)
        {
            for (const auto& [key, b] : Buffers)
            {
                if (key.ViewId == 0)
                    fn(key.NameId, b);
            }
        }
        for (const auto& [key, b] : Buffers)
        {
            if (key.ViewId == viewId)
                fn(key.NameId, b);
        }
    }
};

} // namespace GameEngine::Engine::Renderer::Pipeline
