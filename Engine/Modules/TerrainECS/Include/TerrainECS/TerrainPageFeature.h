#pragma once

#include "Engine/Rendering/IRenderFeature.h"
#include "TerrainECS/TerrainPageTableRing.h"
#include "TerrainECS/TerrainPages.h"
#include "TerrainECS/TerrainSurface/TerrainSurfaceStore.h"
#include "Rendering/Core/Handle.h"
#include "Types/FlatMap.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace GameEngine::Rendering
{
class CommandList;
class IDevice;
enum class TextureFormat : uint32_t;
} // namespace GameEngine::Rendering

namespace GameEngine::TerrainECS
{

/// The GPU side of the paged fields: the height cache texture and one texture per surface plane, shared
/// by every paged terrain, and each paged terrain's page-table words. Tables become readable only with
/// the upload pass that writes their pages (CommitLatch); until then readers keep the last committed ones.
class TerrainPageFeature : public Engine::Renderer::IRenderFeature
{
public:
    ~TerrainPageFeature() override;

    /// Main thread: this frame's height and surface uploads (moved out of `pages`) and the words of
    /// every paged terrain whose height or surface table changed since the last publish.
    void Publish(TerrainPages& pages);

    /// Render thread, once per render-graph frame (repeat calls in the same frame do nothing):
    /// takes what was published since the last latch, and frees the staging of uploads the GPU has
    /// finished with.
    void Latch(Rendering::IDevice& device, uint64 rgFrame);
    /// True when latched uploads are staged and wait for an upload pass.
    bool HasStagedUploads() const { return m_Staging.IsValid(); }
    /// Render thread, once the upload pass that will call FlushUploads is declared: the tables
    /// latched with the staged uploads become the readable ones.
    void CommitLatch();
    /// Records the staged uploads: clears the height cache to 0 on first use, copies each slot of
    /// each field and plane, and leaves every written texture in ShaderResource.
    void FlushUploads(Rendering::CommandList& cl);

    /// The cache texture, invalid until a latch after the one whose upload pass cleared it: a
    /// consumer bound in the frame that creates it could run before that pass clears it (a draw
    /// at rest has no edge to the upload pass), so it reads the texture of a later frame, whose
    /// submission follows the clear.
    Rendering::TextureHandle CacheTexture() const { return m_CacheReadable ? m_Cache : Rendering::TextureHandle{}; }
    /// The committed words of paged terrain `terrain` and their version; false when it is not paged.
    bool LatchedTable(uint32 terrain, std::span<const uint32>& words, uint64& version) const;
    /// The committed surface words of terrain `terrain` and their version; false when its surface is
    /// not paged.
    bool LatchedSurfaceTable(uint32 terrain, std::span<const uint32>& words, uint64& version) const;
    /// The cache texture of surface plane `plane` for terrain `terrain`. Invalid when the terrain's
    /// committed surface table does not have the plane, and until a latch after the one whose upload
    /// pass first wrote the texture (the rule of CacheTexture).
    Rendering::TextureHandle SurfacePlaneTexture(uint32 terrain, PageStreaming::PageFieldKind plane) const;
    /// Render thread, after Latch: this frame's slot of the page-table ring with terrain `terrain`'s
    /// committed height table, then its surface table when paged. The terrain draw and the grass bind
    /// this one slot. Invalid Buffer when the terrain's height is not paged.
    TerrainPageTableRing::Slot BindTableRing(uint32 terrain, uint32 generation);
    /// A paged terrain's height table at a publish: its version, its word count and TerrainPages::WholeTableHash.
    struct PublishedTable
    {
        uint32 Terrain = 0;
        uint64 Version = 0;
        uint32 Words = 0;
        uint64 Hash = 0;
    };
    /// The tables at the last publish before frame `Frame`, and the ring slot that frame bound, copied on the GPU.
    /// A slot whose version equals its table's version holds the table's hash.
    struct TableReport
    {
        uint64 Frame = 0; // the render-graph frame of the copy
        std::vector<PublishedTable> Tables;
        TerrainPageTableRing::SlotReport Slot;
    };
    /// Main thread, on request: the next publish hashes the tables and the next frame copies its ring slot.
    /// The report completes when the GPU finishes that frame, so a reader gets it a frame or more later.
    void RequestTableReport();
    /// Main thread: the last completed report; false before the first.
    bool LastTableReport(TableReport& out) const;
    /// Render thread, after Latch: reads a completed copy, and declares the copy of a requested report.
    /// No work when no report is requested.
    void DeclareTableReport(Rendering::RenderGraph::RGFrame& frame);
    void OnFrameSubmittedRG(Rendering::RenderGraph::RGFrame& frame,
                            const Rendering::IDevice::GpuSyncToken& token) override;
    void OnFrameStreamRetiredRG(Rendering::RenderGraph::RGFrame& frame) override;
    /// Render thread: the serial of the last committed page change (an entry, a fade or an upload) of
    /// terrain `terrain` that meets terrain-UV rect `uv` (X along U, Y along V), to one cell of
    /// kChangeGridCells a side; 0 before the first. The table versions change for every page instead.
    uint64 ChangeSerialOver(uint32 terrain, const Mathematics::Rect& uv) const;
    /// Bumped when the device was rebuilt: the main side restarts its cache (every page re-uploads).
    uint64 DeviceEpoch() const { return m_DeviceEpoch.load(std::memory_order_acquire); }

    void OnDeviceRebuilt(Rendering::IDevice* device) override;

private:
    // The snapshots of one table that no set holds any more, kept for Publish to bring up to date.
    struct TablePool
    {
        std::mutex Mutex;
        std::vector<std::unique_ptr<PackedPageTable>> Free;
    };
    // Snapshots are shared, not copied, between the published, latched and committed sets: Publish
    // copies a table once when its version changes, into a pooled snapshot that takes only the words
    // changed since its own version.
    struct Table
    {
        std::shared_ptr<const PackedPageTable> Snapshot;
        uint32 PlaneMask = 0;            // a surface table's planes (TerrainSurfacePageSource::PlaneMask)
        std::shared_ptr<TablePool> Pool; // Publish's: where this table's released snapshots return
    };
    using TableMap = std::unordered_map<uint32, Table>;
    struct TableSet
    {
        TableMap Height;
        TableMap Surface;
    };
    struct RetiringBuffer
    {
        Rendering::BufferHandle Buffer;
        uint64 Frame = 0;
    };
    // One plane's cache texture. A slot is read only after its upload, so the texture is never cleared:
    // a BC5 image cannot take a clear.
    struct PlaneCache
    {
        Rendering::TextureHandle Texture;
        uint32 SlotsPerRow = 0; // the field's slots per row when the texture was made
        bool Written = false;   // FlushUploads recorded an upload into it
        bool Readable = false; // an upload into it precedes this latch
    };
    // The report that the GPU copies now: declared in a frame, then sealed at that frame's submit.
    struct ReportCopy
    {
        enum class Stage : uint8
        {
            None,
            Declared,
            Sealed
        };
        Stage Step = Stage::None;
        const Rendering::RenderGraph::RGFrame* FrameKey = nullptr;
        uint64 Frame = 0;
        bool Recorded = false; // the copy pass recorded the copy
        Rendering::IDevice::GpuSyncToken Token;
        std::vector<PublishedTable> Tables;
    };

    static void PublishTable(Table& table, const PackedPageTable* source, uint32 planeMask);
    // A snapshot's deleter: returns it to `pool`, or frees it when the pool is full.
    static void ReturnSnapshot(const std::shared_ptr<TablePool>& pool, const PackedPageTable* snapshot);
    static void ForgetUnpaged(TableMap& tables, std::span<const uint32> paged);
    static const PackedPageTable* FindTable(const TableMap& tables, uint32 terrain);
    void EnsureCache();
    void EnsurePlaneCaches();
    void FlushSurfaceUploads(Rendering::CommandList& cl);
    Rendering::TextureHandle CreateCacheTexture(uint32 dim, Rendering::TextureFormat format, const char* name);
    // Every copy into `texture` on `cl` is recorded: it returns to ShaderResource.
    static void EndCopies(Rendering::CommandList& cl, Rendering::TextureHandle texture);
    void StageUploads();
    void RetireStaging();
    // Marks the latched change rects with a new serial: called where the latched tables commit.
    void CommitChanges();
    // Hashes a whole pack of each table in `paged` for the requested report.
    void PublishTableHashes(const TerrainPages& pages, std::span<const uint32> paged);
    // Makes the sealed copy the last report once the GPU completes it.
    void CompleteTableReport();

    // Published by the main thread, under m_Mutex.
    std::mutex m_Mutex;
    std::vector<HeightPageUpload> m_PublishedUploads;
    std::vector<SurfacePageUpload> m_PublishedSurfaceUploads;
    TableSet m_PublishedTables;
    std::vector<PageChangeRect> m_PublishedChanges;
    uint32 m_CacheDim = 0;
    uint32 m_SurfaceSlotsPerRow = 0;

    // Render thread only.
    Rendering::IDevice* m_Device = nullptr;
    std::vector<HeightPageUpload> m_LatchedUploads;
    std::vector<SurfacePageUpload> m_LatchedSurfaceUploads;
    std::vector<std::size_t> m_SurfaceStagingOffsets; // each latched surface upload's bytes in m_Staging
    TableSet m_LatchedTables;                          // latched with the latched uploads
    std::vector<PageChangeRect> m_LatchedChanges;      // commit with m_LatchedTables
    TableSet m_CommittedTables;                        // what LatchedTable and LatchedSurfaceTable read
    Rendering::BufferHandle m_Staging;                 // the latched uploads, staged
    std::vector<RetiringBuffer> m_Retiring;
    Rendering::TextureHandle m_Cache;
    bool m_CacheCleared = false;  // its clear is recorded (FlushUploads)
    bool m_CacheReadable = false; // its clear was recorded before this latch
    // Per paged terrain, by render index: the serial of the last committed change in each cell of its
    // UV, kChangeGridCells x kChangeGridCells, row-major in V.
    static constexpr uint32 kChangeGridCells = 64u;
    FlatMap<uint32, std::vector<uint64>> m_ChangeGrids;
    uint64 m_ChangeSerial = 0;
    TerrainPageTableRing m_TableRing;
    std::array<PlaneCache, kSurfacePlanes.size()> m_Planes;
    uint32 m_LatchedCacheDim = 0;
    uint32 m_LatchedSurfaceSlotsPerRow = 0;
    uint64 m_LatchedFrame = ~0ull;
    std::atomic<uint64> m_DeviceEpoch{0};

    // The table report: requested and read on the main thread, copied on the render thread.
    std::atomic<bool> m_ReportRequested{false};
    mutable std::mutex m_ReportMutex; // guards the published hashes and the last report
    std::vector<PublishedTable> m_PublishedTableHashes;
    bool m_TableHashesPublished = false;
    TableReport m_LastReport;
    bool m_HasReport = false;
    ReportCopy m_ReportCopy; // render thread only
};

} // namespace GameEngine::TerrainECS
