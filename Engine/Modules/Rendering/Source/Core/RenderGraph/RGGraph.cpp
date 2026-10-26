#include "Rendering/Core/RenderGraph/RGGraph.h"

#include "Rendering/Core/RenderGraph/RGScratch.h"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace GameEngine::Rendering::RenderGraph
{

RGGraph::RGGraph()
    : m_Arena("RenderGraph.Frame")
{
    m_Resources.reserve(256);
    m_ResourceExternal.reserve(256);
    m_Passes.reserve(128);
    m_Accesses.reserve(512);
    m_ResourceNeeded.reserve(256);
}

void RGGraph::BeginFrame()
{
    m_Resources.clear();
    m_ResourceExternal.clear();
    m_ResourceImported.clear();
    m_ResourceInitLayout.clear();
    m_ResourceExportLayout.clear();
    m_ResourceCrossFramePhysical.clear();
    m_Passes.clear();
    m_Accesses.clear();
    m_OrderingEdges.clear();
    m_Barriers.clear();
    m_BarrierBatches.clear();
    m_Submissions.clear();
    m_Cells.clear();
    m_CellOff.clear();
    m_CellMips.clear();
    m_PassAccValid = false;
    // m_QueueTimelineNext deliberately NOT reset: in-flight frames overlap and
    // per-frame values would collide with frame N-1's still-pending signals.
    m_ResourceNeeded.clear();
    m_ResourceUsedByLive.clear();
    m_LivePassCount = 0;
    m_ScheduledOrder.clear();
    m_Level.clear();
    m_WorkingSetId.clear();
    m_WorkingSetTransitions = 0;
    m_Arena.Reset();
}

const char* RGGraph::CopyName(const char* name)
{
    if (!name)
        return nullptr;
    const size_t len = std::strlen(name) + 1;
    char* dst = static_cast<char*>(m_Arena.Allocate(len, 1));
    std::memcpy(dst, name, len);
    return dst;
}

void RGGraph::EnsurePassAccessIndex()
{
    if (m_PassAccValid)
        return;
    const uint32_t passCount = static_cast<uint32_t>(m_Passes.size());
    m_PassAccOff.assign(passCount + 1, 0);
    for (const RGAccessRecord& a : m_Accesses)
        if (a.Pass < passCount)
            ++m_PassAccOff[a.Pass + 1];
    for (uint32_t p = 0; p < passCount; ++p)
        m_PassAccOff[p + 1] += m_PassAccOff[p];
    m_PassAccIdx.resize(m_Accesses.size());
    for (uint32_t i = 0; i < m_Accesses.size(); ++i)
    {
        const RGPassId p = m_Accesses[i].Pass;
        if (p < passCount)
            m_PassAccIdx[m_PassAccOff[p]++] = i;
    }
    // Shift offsets back after using them as cursors.
    for (uint32_t p = passCount; p > 0; --p)
        m_PassAccOff[p] = m_PassAccOff[p - 1];
    m_PassAccOff[0] = 0;
    m_PassAccValid = true;
}

RGResourceId RGGraph::CreateResource(const RGResourceDesc& desc)
{
    const RGResourceId id = static_cast<RGResourceId>(m_Resources.size());
    m_Resources.push_back(desc);
    m_Resources.back().Name = CopyName(desc.Name);
    m_ResourceExternal.push_back(0);
    m_ResourceImported.push_back(0);
    m_ResourceInitLayout.push_back(RGImageLayout::Undefined);
    m_ResourceExportLayout.push_back(RGImageLayout::Undefined);
    m_ResourceCrossFramePhysical.push_back(0);
    return id;
}

void RGGraph::SetExportLayout(RGResourceId id, RGImageLayout layout)
{
    assert(id < m_Resources.size() && "SetExportLayout: unknown resource");
    assert(m_Resources[id].Kind == RGResourceKind::Texture && "SetExportLayout: textures only");
    assert((layout == RGImageLayout::ShaderReadOnly || layout == RGImageLayout::General) &&
           "SetExportLayout: unsupported consumer contract");
    m_ResourceExportLayout[id] = layout;
}

void RGGraph::MarkExternal(RGResourceId id)
{
    assert(id < m_Resources.size() && "MarkExternal: unknown resource");
    m_ResourceExternal[id] = 1;
}

void RGGraph::MarkImported(RGResourceId id, RGImageLayout initialLayout)
{
    assert(id < m_Resources.size() && "MarkImported: unknown resource");
    m_ResourceImported[id] = 1;
    m_ResourceInitLayout[id] = initialLayout;
    m_ResourceExternal[id] = 1; // imported resources are sinks/sources -> not culled
}

void RGGraph::SetCrossFramePhysical(RGResourceId id, uint64_t physical)
{
    assert(id < m_Resources.size() && "SetCrossFramePhysical: unknown resource");
    assert(m_ResourceImported[id] != 0 && "SetCrossFramePhysical: imported resources only");
    m_ResourceCrossFramePhysical[id] = physical;
}

RGPassId RGGraph::AddPass(const RGPassDesc& desc)
{
    const RGPassId id = static_cast<RGPassId>(m_Passes.size());
    RGPassRec rec;
    rec.Desc = desc;
    rec.Desc.Name = CopyName(desc.Name);
    m_Passes.push_back(rec);
    return id;
}

void RGGraph::Read(RGPassId pass, RGResourceId resource, RGAccess access, RGRange range)
{
    assert(pass < m_Passes.size() && "Read: unknown pass");
    assert(resource < m_Resources.size() && "Read: unknown resource");
    assert(!IsWrite(access) && "Read: access is a write kind");
    m_Accesses.push_back(RGAccessRecord{pass, resource, access, range});
    m_PassAccValid = false; // CSR index covers a snapshot; new accesses invalidate it
}

void RGGraph::Write(RGPassId pass, RGResourceId resource, RGAccess access, RGRange range)
{
    assert(pass < m_Passes.size() && "Write: unknown pass");
    assert(resource < m_Resources.size() && "Write: unknown resource");
    assert(IsWrite(access) && "Write: access is a read kind");
    m_Accesses.push_back(RGAccessRecord{pass, resource, access, range});
    m_PassAccValid = false; // CSR index covers a snapshot; new accesses invalidate it
}

void RGGraph::WriteEveryPixel(RGPassId pass, RGResourceId resource)
{
    assert(pass < m_Passes.size() && "WriteEveryPixel: unknown pass");
    assert(resource < m_Resources.size() && "WriteEveryPixel: unknown resource");
    assert(m_Resources[resource].Kind == RGResourceKind::Texture && m_Resources[resource].MipLevels == 1 &&
           m_Resources[resource].ArrayLayers == 1 && "WriteEveryPixel: needs a one-mip, one-layer texture");
    m_Accesses.push_back(RGAccessRecord{pass, resource, RGAccess::ColorAttachment, RGRange::All(), true});
    m_PassAccValid = false; // CSR index covers a snapshot; new accesses invalidate it
}

void RGGraph::PreventCulling(RGPassId pass)
{
    assert(pass < m_Passes.size() && "PreventCulling: unknown pass");
    m_Passes[pass].PreventCull = true;
}

void RGGraph::AddOrderingEdge(RGPassId before, RGPassId after)
{
    assert(before < m_Passes.size() && after < m_Passes.size() &&
           "AddOrderingEdge: unknown pass");
    m_OrderingEdges.emplace_back(before, after);
}

void RGGraph::Compile()
{
    const size_t passCount = m_Passes.size();
    const size_t resCount = m_Resources.size();

    // A resource is "needed" if it is an external sink; passes feeding needed
    // resources become needed transitively (backward reachability). This is the
    // Frostbite/RDG ref-count cull: anything that does not reach a sink is dropped.
    m_ResourceNeeded.assign(resCount, 0);
    for (size_t r = 0; r < resCount; ++r)
        m_ResourceNeeded[r] = m_ResourceExternal[r];

    // Dense-indexed adjacency rebuilt from the flat access list (member scratch:
    // capacity retained across frames).
    CollectLiveWriters();

    m_ScratchPassNeeded.assign(passCount, 0);
    m_ScratchWorklist.clear();
    m_ScratchWorklist.reserve(passCount);

    auto markPass = [&](RGPassId p)
    {
        if (!m_ScratchPassNeeded[p])
        {
            m_ScratchPassNeeded[p] = 1;
            m_ScratchWorklist.push_back(p);
        }
    };

    // Seed: prevent-cull passes, and any pass writing an already-needed (external) resource.
    for (RGPassId p = 0; p < passCount; ++p)
    {
        if (m_Passes[p].PreventCull)
            markPass(p);
    }
    for (RGResourceId r = 0; r < resCount; ++r)
    {
        if (m_ResourceNeeded[r])
            for (RGPassId w : m_ScratchWritersOf[r])
                markPass(w);
    }

    // Propagate: a needed pass's reads become needed; their writers become needed.
    while (!m_ScratchWorklist.empty())
    {
        const RGPassId p = m_ScratchWorklist.back();
        m_ScratchWorklist.pop_back();
        for (RGResourceId r : m_ScratchReadsOf[p])
        {
            if (!m_ResourceNeeded[r])
            {
                m_ResourceNeeded[r] = 1;
                for (RGPassId w : m_ScratchWritersOf[r])
                    markPass(w);
            }
        }
    }

    m_LivePassCount = 0;
    for (RGPassId p = 0; p < passCount; ++p)
    {
        if (m_ScratchPassNeeded[p])
        {
            m_Passes[p].Culled = false;
            m_Passes[p].CullReason = RGCullReason::NotCulled;
            ++m_LivePassCount;
        }
        else
        {
            m_Passes[p].Culled = true;
            // Distinguish the MCP "why didn't this run" answers: outputs that DO
            // have readers — all of which were culled — are ProducerCulled;
            // outputs nobody reads (and no sink) are NoConsumer.
            bool anyOutputRead = false;
            for (uint32_t k = 0; k < m_Accesses.size() && !anyOutputRead; ++k)
            {
                const RGAccessRecord& a = m_Accesses[k];
                if (a.Pass != p || !IsWrite(a.Access) || !m_ScratchResHasReader[a.Resource])
                    continue;
                // Self-reads (the Load-derived read of a pass's own attachment)
                // are not consumption — only an OTHER pass's read classifies
                // this as ProducerCulled.
                for (const RGAccessRecord& r : m_Accesses)
                {
                    if (!IsWrite(r.Access) && r.Resource == a.Resource && r.Pass != p)
                    {
                        anyOutputRead = true;
                        break;
                    }
                }
            }
            if (m_ScratchPassOverwritten[p])
                m_Passes[p].CullReason = RGCullReason::Overwritten;
            else
                m_Passes[p].CullReason =
                    anyOutputRead ? RGCullReason::ProducerCulled : RGCullReason::NoConsumer;
        }
    }

    // Realization gate: everything a live pass touches must be allocated, even
    // when no downstream consumer makes it "needed" (e.g. a depth attachment
    // nobody samples).
    m_ResourceUsedByLive.assign(resCount, 0);
    for (const RGAccessRecord& a : m_Accesses)
        if (!m_Passes[a.Pass].Culled)
            m_ResourceUsedByLive[a.Resource] = 1;
}

// Fills the cull's adjacency: each pass's reads, and each resource's writers whose write some pass
// can see. A write is dead when the next pass to touch the resource writes every pixel of it and
// reads none of it; that writer is left out and marked for RGCullReason::Overwritten.
void RGGraph::CollectLiveWriters()
{
    const size_t passCount = m_Passes.size();
    const size_t resCount = m_Resources.size();
    PrepareNested(m_ScratchWritersOf, resCount);
    PrepareNested(m_ScratchReadsOf, passCount);
    PrepareNested(m_ScratchTouchesOf, resCount);
    m_ScratchResHasReader.assign(resCount, 0);
    m_ScratchPassOverwritten.assign(passCount, 0);
    for (const RGAccessRecord& a : m_Accesses)
    {
        const bool write = IsWrite(a.Access);
        if (!write)
        {
            m_ScratchReadsOf[a.Pass].push_back(a.Resource);
            m_ScratchResHasReader[a.Resource] = 1;
        }
        std::vector<RGPassTouch>& touches = m_ScratchTouchesOf[a.Resource];
        if (touches.empty() || touches.back().Pass != a.Pass)
            touches.push_back(RGPassTouch{a.Pass});
        RGPassTouch& touch = touches.back();
        touch.Reads = touch.Reads || !write;
        touch.Writes = touch.Writes || write;
        touch.WritesEveryPixel = touch.WritesEveryPixel || a.WritesEveryPixel;
    }

    for (RGResourceId r = 0; r < resCount; ++r)
    {
        std::vector<RGPassTouch>& touches = m_ScratchTouchesOf[r];
        // RGFrame declares in pass order; a raw graph may not, and then the touches are sorted and
        // a pass's split entries merged.
        const auto earlier = [](const RGPassTouch& a, const RGPassTouch& b) { return a.Pass < b.Pass; };
        if (!std::is_sorted(touches.begin(), touches.end(), earlier))
        {
            std::stable_sort(touches.begin(), touches.end(), earlier);
            size_t merged = 0;
            for (size_t i = 1; i < touches.size(); ++i)
            {
                RGPassTouch& last = touches[merged];
                if (touches[i].Pass != last.Pass)
                {
                    touches[++merged] = touches[i];
                    continue;
                }
                last.Reads = last.Reads || touches[i].Reads;
                last.Writes = last.Writes || touches[i].Writes;
                last.WritesEveryPixel = last.WritesEveryPixel || touches[i].WritesEveryPixel;
            }
            touches.resize(merged + 1);
        }
        for (size_t i = 0; i < touches.size(); ++i)
        {
            if (!touches[i].Writes)
                continue;
            const bool overwritten = i + 1 < touches.size() && touches[i + 1].WritesEveryPixel &&
                                     !touches[i + 1].Reads;
            if (overwritten)
                m_ScratchPassOverwritten[touches[i].Pass] = 1;
            else
                m_ScratchWritersOf[r].push_back(touches[i].Pass);
        }
    }
}

} // namespace GameEngine::Rendering::RenderGraph
