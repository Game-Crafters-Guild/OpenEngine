#pragma once

// Test-only read of UIManager's SDF primitive ring. Friended by UIManager (see the
// `friend struct UIManagerSdfRingAccess` declaration there) so the ring sizing tests can
// read the capacity the first frame allocated and the capacity after growth.

#include "UIOpaqueInteriors.h"

#include "UI/UIFrameBufferRing.h"
#include "UI/UIManager.h"
#include "UI/UIPrimitive.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace GameEngine
{

struct UIManagerSdfRingAccess
{
    // Elements per frame-in-flight slice; 0 before the first frame created the ring.
    static size_t PrimitiveCapacity(const UIManager& ui)
    {
        return ui.m_SdfPrimRing ? ui.m_SdfPrimRing->GetCapacity() : 0;
    }
    static size_t PrimitiveSlots(const UIManager& ui) { return ui.m_PrimitiveAllocator.GetTotalSlots(); }
    static size_t OpaqueInteriorCount(const UIManager& ui) { return ui.m_SdfOpaqueInteriors->Interiors().size(); }
    // The state of a build staged without the pre-pass shaders: the main draw runs alone.
    static void DisableOpaquePrePass(UIManager& ui)
    {
        ui.m_SdfOpaqueAvailable = false;
        ui.m_SdfOpaqueEncodedAvailable = false;
    }
    // The linear main draw's description, which a pass with no depth attachment interns.
    static const Rendering::PipelineDesc& MainDrawDesc(const UIManager& ui) { return ui.m_SdfPipelineDesc; }
    // The main draws interned over the pre-pass depth.
    static size_t OverPrePassPipelineCount(const UIManager& ui)
    {
        size_t count = 0;
        for (const Rendering::GraphicsPipelineId id : ui.m_SdfPipelineIdOverPrePass)
            count += id.IsValid() ? 1 : 0;
        return count;
    }
    // Lists the interiors again from primitives with the RGB of `fill`, so only the pre-pass
    // paints `fill`. The list ends at the manager's source versions with a version
    // past the old list's, so the next frame uploads it and does not rebuild it.
    static void PaintOpaqueInteriors(UIManager& ui, uint32_t fill)
    {
        std::vector<UI::UIPrimitive> painted = ui.m_PersistentPrimitives;
        for (UI::UIPrimitive& prim : painted)
            prim.FillColor = (prim.FillColor & 0xFF000000u) | (fill & 0x00FFFFFFu);
        UI::UIOpaqueInteriors::Sources sources{painted, ui.m_PersistentClipRects, ui.m_DrawOrder,
                                               ui.m_PrimitiveStoreVersion, ui.m_ClipStoreVersion,
                                               ui.m_DrawOrderStoreVersion};
        auto interiors = std::make_unique<UI::UIOpaqueInteriors>();
        while (interiors->Version() <= ui.m_SdfOpaqueInteriors->Version())
        {
            UI::UIOpaqueInteriors::Sources moved = sources;
            ++moved.PrimitiveVersion;
            interiors->Refresh(moved);
            interiors->Refresh(sources);
        }
        ui.m_SdfOpaqueInteriors = std::move(interiors);
    }
};

} // namespace GameEngine
