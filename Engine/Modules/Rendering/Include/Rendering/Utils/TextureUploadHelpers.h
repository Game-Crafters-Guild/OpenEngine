#pragma once

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <new>
#include <optional>
#include <span>
#include <vector>

namespace GameEngine
{
namespace Rendering
{

// Minimal, header-only helpers for uploading pixel data to GPU textures via
// transient staging buffers. Follows the same pattern as BufferHelpers.h.
//
// GPU safety contract: after ExecuteCommandLists returns, each staging buffer
// is destroyed via IDevice::DestroyBuffer. Backends are expected to defer
// actual destruction until the GPU has finished referencing the buffer. The
// Vulkan backend implements this via timeline-semaphore or per-frame-fence
// deferral (see VulkanDevice::DestroyBuffer). If a backend does NOT defer,
// it must execute command lists synchronously before DestroyBuffer returns.

struct TextureUploadEntry
{
    TextureHandle Texture;
    const void* Pixels;
    uint32_t Width;
    uint32_t Height;
    uint32_t Depth = 1;
    size_t RowPitchBytes;       // bytes per row (e.g. Width * 4 for RGBA8)
    size_t SlicePitchBytes = 0; // bytes per depth slice (0 = Height * RowPitchBytes)

    // State the texture is in when this copy begins. Undefined is right for a
    // texture that was just created and that nothing has sampled yet.
    //
    // TRAP: Undefined resolves to srcStage = TOP_OF_PIPE, whose first
    // synchronization scope is empty. Re-uploading into a texture the GPU is
    // already sampling therefore gets NO write-after-read dependency on the
    // in-flight frame's reads, and the copy may land mid-sample. Repeat uploads
    // must name ShaderResource — see ReuploadTexture2D.
    ResourceState SourceState = ResourceState::Undefined;
};

namespace Detail
{
constexpr size_t kUploadEntryStackCapacity = 4;
constexpr size_t kD3D12TextureDataPitchAlignment = 256; // matches D3D12_TEXTURE_DATA_PITCH_ALIGNMENT

inline size_t AlignUp(size_t value, size_t alignment)
{
    return (value + alignment - 1u) & ~(alignment - 1u);
}
} // namespace Detail

// Upload one or more 2D or 3D textures in a single command list submission.
// Each texture must already be created with TextureUsage::TransferDst.
// Source layout is each entry's SourceState; final state is ShaderResource.
//
// D3D12 callers can supply any RowPitch/SlicePitch; this helper repacks the
// source into a tightly-packed, pitch-aligned staging buffer transparently.
// False means preparation failed and no command list was submitted. True means
// the complete batch was handed to ExecuteCommandLists; that void device API
// does not report submission/completion success. Device health/reprovision owns
// recovery after dispatch, so callers must not treat true as GPU completion.
inline bool UploadTexturesBatched(IDevice* device,
                                  const TextureUploadEntry* entries,
                                  size_t count,
                                  const char* stagingDebugName = "TexUploadStaging")
{
    if (!device || !entries || count == 0)
        return false;

    for (size_t i = 0; i < count; ++i)
        if (!entries[i].Texture.IsValid() || !entries[i].Pixels ||
            !entries[i].Width || !entries[i].Height || !entries[i].RowPitchBytes)
            return false;

    const bool isD3D12 = device->GetAPI() == GraphicsAPI::DirectX12;
    // Row-pitch alignment for buffer->texture copies: D3D12 and WebGPU both
    // require 256 (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT / COPY_BYTES_PER_ROW_ALIGNMENT);
    // Vulkan/Metal report 1.
    const size_t rowPitchAlignment = isD3D12
                                         ? Detail::kD3D12TextureDataPitchAlignment
                                         : std::max<size_t>(1, device->GetCapabilities().textureCopyRowPitchAlignment);

    // Per-entry scratch state. Common case (≤4 textures) stays on the stack.
    struct EntryScratch
    {
        BufferHandle Staging;
        size_t UploadRowPitch = 0;
        size_t UploadSlicePitch = 0;
    };
    std::array<EntryScratch, Detail::kUploadEntryStackCapacity> stackScratch{};
    std::vector<EntryScratch> heapScratch;
    EntryScratch* scratch = stackScratch.data();
    if (count > Detail::kUploadEntryStackCapacity)
    {
        try
        {
            heapScratch.resize(count);
        }
        catch (const std::bad_alloc&)
        {
            return false;
        }
        scratch = heapScratch.data();
    }

    struct StagingCleanup
    {
        IDevice* Device;
        EntryScratch* Entries;
        size_t Count;
        ~StagingCleanup()
        {
            for (size_t i = 0; i < Count; ++i)
                if (Entries[i].Staging.IsValid())
                    Device->DestroyBuffer(Entries[i].Staging);
        }
    } cleanup{device, scratch, count};

    // Reusable padding buffer when D3D12 row-pitch alignment forces a copy.
    // Kept outside the loop so allocations amortize across the batch.
    std::vector<unsigned char> padded;
    std::unique_ptr<CommandList> cl;
    std::vector<CommandList*> commandLists;

    try
    {
        for (size_t i = 0; i < count; ++i)
        {
            const auto& entry = entries[i];
            const uint32_t depth = entry.Depth > 0 ? entry.Depth : 1u;
            const size_t sourceSlicePitch =
                entry.SlicePitchBytes ? entry.SlicePitchBytes : static_cast<size_t>(entry.Height) * entry.RowPitchBytes;
            scratch[i].UploadRowPitch = entry.RowPitchBytes;
            scratch[i].UploadSlicePitch = sourceSlicePitch;

            const void* uploadPixels = entry.Pixels;
            if (rowPitchAlignment > 1)
            {
                scratch[i].UploadRowPitch = Detail::AlignUp(entry.RowPitchBytes, rowPitchAlignment);
                scratch[i].UploadSlicePitch = scratch[i].UploadRowPitch * entry.Height;

                const bool rowPitchPadded = scratch[i].UploadRowPitch != entry.RowPitchBytes;
                const bool slicePitchPadded = scratch[i].UploadSlicePitch != sourceSlicePitch;
                if (rowPitchPadded || slicePitchPadded)
                {
                    padded.assign(static_cast<size_t>(depth) * scratch[i].UploadSlicePitch, 0);
                    const auto* src = static_cast<const unsigned char*>(entry.Pixels);
                    for (uint32_t z = 0; z < depth; ++z)
                    {
                        for (uint32_t y = 0; y < entry.Height; ++y)
                        {
                            const size_t srcOffset =
                                static_cast<size_t>(z) * sourceSlicePitch + static_cast<size_t>(y) * entry.RowPitchBytes;
                            const size_t dstOffset =
                                static_cast<size_t>(z) * scratch[i].UploadSlicePitch + static_cast<size_t>(y) * scratch[i].UploadRowPitch;
                            std::memcpy(padded.data() + dstOffset, src + srcOffset, entry.RowPitchBytes);
                        }
                    }
                    uploadPixels = padded.data();
                }
            }

            const size_t uploadSize = static_cast<size_t>(depth) * scratch[i].UploadSlicePitch;
            scratch[i].Staging = device->CreateUploadBuffer(uploadSize, stagingDebugName);
            if (!scratch[i].Staging.IsValid())
                return false;
            device->UpdateBuffer(scratch[i].Staging, 0, uploadSize, uploadPixels);
        }

        cl = device->CreateCommandList(IDevice::QueueType::Graphics);
        if (!cl)
            return false;
        cl->Begin();
        for (size_t i = 0; i < count; ++i)
        {
            if (!scratch[i].Staging.IsValid())
                continue;
            const auto& entry = entries[i];
            const uint32_t depth = entry.Depth > 0 ? entry.Depth : 1u;
            cl->Barrier(ResourceBarrier::CreateTextureBarrier(
                entry.Texture, entry.SourceState, ResourceState::CopyDest));
            cl->CopyBufferToTextureSubresource(
                scratch[i].Staging, entry.Texture, 0, 0,
                entry.Width, entry.Height, 0, scratch[i].UploadRowPitch,
                depth, scratch[i].UploadSlicePitch);
            cl->Barrier(ResourceBarrier::CreateTextureBarrier(
                entry.Texture, ResourceState::CopyDest, ResourceState::ShaderResource));
        }
        cl->End();
        commandLists.push_back(cl.get());
    }
    catch (const std::bad_alloc&)
    {
        return false;
    }

    // Exceptions after entering the device's void dispatch cannot establish
    // whether any copy ran. Propagate them; never report a pre-submit failure.
    device->ExecuteCommandLists(commandLists);

    // Cleanup defers actual staging deallocation through the backend. No copy
    // is submitted until every staging allocation and the command list exist.
    return true;
}

// Upload pixel data to a single existing 2D texture via a transient staging buffer.
// Texture must already be created with TextureUsage::TransferDst.
// Initial layout is assumed Undefined; final state is ShaderResource.
inline void UploadTexture2D(IDevice* device,
                            TextureHandle tex,
                            const void* pixels,
                            uint32_t width, uint32_t height,
                            size_t rowPitchBytes,
                            const char* stagingDebugName = "TexUploadStaging")
{
    TextureUploadEntry entry{tex, pixels, width, height, 1, rowPitchBytes, 0};
    UploadTexturesBatched(device, &entry, 1, stagingDebugName);
}

// Re-upload into a 2D texture that is already resident in ShaderResource and
// may still be sampled by an in-flight frame (video frames, dynamic previews).
// The copy is ordered after prior fragment reads by naming ShaderResource as
// the source state; UploadTexture2D's Undefined source would not order it at
// all (see TextureUploadEntry::SourceState).
//
// The texture must be created with TransferDst and reach ShaderResource before
// the first call — TextureDesc::initialState does that at creation time.
inline void ReuploadTexture2D(IDevice* device,
                              TextureHandle tex,
                              const void* pixels,
                              uint32_t width, uint32_t height,
                              size_t rowPitchBytes,
                              const char* stagingDebugName)
{
    TextureUploadEntry entry{tex, pixels, width, height, 1, rowPitchBytes, 0,
                             ResourceState::ShaderResource};
    UploadTexturesBatched(device, &entry, 1, stagingDebugName);
}

// One mip level of a 2D texture, tightly packed. For block-compressed formats
// SizeBytes is the exact per-level payload (e.g. ktxTexture_GetImageSize);
// Width/Height are the level's texel dimensions (not block counts).
struct TextureMipUploadEntry
{
    const void* Pixels;
    size_t SizeBytes;
    uint32_t Width;
    uint32_t Height;
};

// Where one mip level sits in a staging buffer.
struct TextureMipStagingLevel
{
    size_t Offset = 0;        // byte offset of the level in the staging buffer
    size_t RowPitch = 0;      // staging row pitch in bytes; 0 = tightly packed (block-compressed)
    size_t TightRowPitch = 0; // Width * bytes per pixel; 0 for block-compressed
    size_t SizeBytes = 0;     // the level's tight payload size
    size_t StagedBytes = 0;   // bytes the level occupies in staging (SizeBytes, or RowPitch * Height when padded)
    uint32_t Width = 0;
    uint32_t Height = 0;

    // True when rows must be written one by one: the staging pitch is wider than the payload's.
    bool HasPaddedRows() const { return RowPitch != 0 && RowPitch != TightRowPitch; }
};

// One staging buffer for a 2D mip chain (layer 0), laid out by the device's copy
// rules. Empty Levels = the device cannot take the format (see PlanTextureMipStaging).
struct TextureMipStagingPlan
{
    TextureFormat Format = TextureFormat::Unknown;
    std::vector<TextureMipStagingLevel> Levels;
    size_t TotalBytes = 0;

    bool IsValid() const { return !Levels.empty() && TotalBytes != 0; }
};

// A texture whose staging buffer holds every planned level, ready to record.
struct StagedTextureUpload
{
    TextureHandle Texture;
    BufferHandle Staging;
    TextureMipStagingPlan Plan;
};

namespace Detail
{
// A mip level's tight payload size and texel dimensions, without its bytes: what
// PlanTextureMipStaging needs to place the level.
struct TextureMipExtent
{
    size_t SizeBytes;
    uint32_t Width;
    uint32_t Height;
};

// Plan one staging buffer for a 2D mip chain. Each level starts 16-byte aligned (Vulkan's texel-block
// rule; 512 on D3D12), and uncompressed rows use the device's copy row pitch. The plan is invalid for
// a format with no per-pixel size and for a block-compressed format on D3D12 (no DXGI mapping yet).
inline TextureMipStagingPlan PlanTextureMipStaging(const IDevice& device,
                                                   TextureFormat format,
                                                   std::span<const TextureMipExtent> levels)
{
    TextureMipStagingPlan plan;
    plan.Format = format;
    const bool isBlock = IsBlockCompressedFormat(format);
    const uint32_t bpp = BytesPerPixel(format);
    const bool isD3D12 = device.GetAPI() == GraphicsAPI::DirectX12;
    if (levels.empty() || (isBlock && isD3D12) || (!isBlock && bpp == 0))
        return plan;

    const size_t rowPitchAlignment = isD3D12
                                         ? kD3D12TextureDataPitchAlignment
                                         : std::max<size_t>(1, device.GetCapabilities().textureCopyRowPitchAlignment);
    constexpr size_t kVulkanMipOffsetAlignment = 16;
    constexpr size_t kD3D12TextureDataPlacementAlignment = 512;
    const size_t mipOffsetAlignment = isD3D12 ? kD3D12TextureDataPlacementAlignment : kVulkanMipOffsetAlignment;

    plan.Levels.resize(levels.size());
    for (size_t m = 0; m < levels.size(); ++m)
    {
        TextureMipStagingLevel& level = plan.Levels[m];
        level.Width = levels[m].Width;
        level.Height = levels[m].Height;
        level.SizeBytes = levels[m].SizeBytes;
        level.StagedBytes = levels[m].SizeBytes;
        if (!isBlock)
        {
            level.TightRowPitch = static_cast<size_t>(level.Width) * bpp;
            level.RowPitch = AlignUp(level.TightRowPitch, rowPitchAlignment);
            if (level.HasPaddedRows())
                level.StagedBytes = level.RowPitch * level.Height;
        }
        level.Offset = plan.TotalBytes;
        plan.TotalBytes += AlignUp(level.StagedBytes, mipOffsetAlignment);
    }
    return plan;
}

// Copy one tightly packed level into its planned place in `staging`, padding its
// rows when the plan does.
inline void WriteTextureMipLevel(IDevice& device,
                                 BufferHandle staging,
                                 const TextureMipStagingPlan& plan,
                                 uint32_t level,
                                 const void* pixels)
{
    const TextureMipStagingLevel& placed = plan.Levels[level];
    if (!placed.HasPaddedRows())
    {
        device.UpdateBuffer(staging, placed.Offset, placed.SizeBytes, pixels);
        return;
    }
    std::vector<unsigned char> padded(placed.StagedBytes, 0);
    const auto* tight = static_cast<const unsigned char*>(pixels);
    for (uint32_t y = 0; y < placed.Height; ++y)
        std::memcpy(padded.data() + static_cast<size_t>(y) * placed.RowPitch,
                    tight + static_cast<size_t>(y) * placed.TightRowPitch, placed.TightRowPitch);
    device.UpdateBuffer(staging, placed.Offset, padded.size(), padded.data());
}

// Record every upload's copies into one command list: one barrier batch moves every texture's
// levels from Undefined to CopyDest, the copies follow, and a second batch moves them all to
// ShaderResource. SubmitStagedTextureUploads records, submits and releases.
inline void RecordStagedTextureUploads(CommandList& commandList, std::span<const StagedTextureUpload> uploads)
{
    std::vector<ResourceBarrier> toCopyDest;
    std::vector<ResourceBarrier> toShaderResource;
    toCopyDest.reserve(uploads.size());
    toShaderResource.reserve(uploads.size());
    for (const StagedTextureUpload& upload : uploads)
    {
        const auto mipCount = static_cast<uint32_t>(upload.Plan.Levels.size());
        toCopyDest.push_back(ResourceBarrier::CreateTextureBarrier(
            upload.Texture, ResourceState::Undefined, ResourceState::CopyDest, 0, mipCount));
        toShaderResource.push_back(ResourceBarrier::CreateTextureBarrier(
            upload.Texture, ResourceState::CopyDest, ResourceState::ShaderResource, 0, mipCount));
    }

    commandList.BarrierBatch(toCopyDest);
    for (const StagedTextureUpload& upload : uploads)
    {
        for (uint32_t m = 0; m < upload.Plan.Levels.size(); ++m)
        {
            const TextureMipStagingLevel& level = upload.Plan.Levels[m];
            commandList.CopyBufferToTextureSubresource(upload.Staging, upload.Texture, m, 0,
                                                       level.Width, level.Height,
                                                       level.Offset, level.RowPitch,
                                                       1, 0, 0, 0, ResourceState::CopyDest);
        }
    }
    commandList.BarrierBatch(toShaderResource);
}
} // namespace Detail

// Plan one staging buffer for a 2D mip chain and write every level into it. The
// caller sets Texture to the texture the copies go to. Nullopt, with nothing left
// on the device, when the plan is invalid or the device refuses the buffer.
inline std::optional<StagedTextureUpload> StageTextureMips(IDevice& device,
                                                           TextureFormat format,
                                                           std::span<const TextureMipUploadEntry> mips,
                                                           const char* stagingDebugName)
{
    std::vector<Detail::TextureMipExtent> extents(mips.size());
    for (size_t m = 0; m < mips.size(); ++m)
        extents[m] = {mips[m].SizeBytes, mips[m].Width, mips[m].Height};
    StagedTextureUpload upload{{}, {}, Detail::PlanTextureMipStaging(device, format, extents)};
    if (!upload.Plan.IsValid())
        return std::nullopt;

    upload.Staging = device.CreateUploadBuffer(upload.Plan.TotalBytes, stagingDebugName);
    if (!upload.Staging.IsValid())
        return std::nullopt;
    for (uint32_t m = 0; m < mips.size(); ++m)
        Detail::WriteTextureMipLevel(device, upload.Staging, upload.Plan, m, mips[m].Pixels);
    return upload;
}

// Copy every upload with one command list and one submit. False when the device gives no command
// list: the textures stay unwritten. The staging buffers are destroyed in both cases (the backend
// defers that until the GPU is done, see the contract at the top of this header).
inline bool SubmitStagedTextureUploads(IDevice& device, std::span<const StagedTextureUpload> uploads)
{
    if (uploads.empty())
        return true;
    const std::unique_ptr<CommandList> commandList = device.CreateCommandList(IDevice::QueueType::Graphics);
    if (commandList)
    {
        commandList->Begin();
        Detail::RecordStagedTextureUploads(*commandList, uploads);
        commandList->End();
        device.ExecuteCommandLists({commandList.get()});
    }
    for (const StagedTextureUpload& upload : uploads)
        device.DestroyBuffer(upload.Staging);
    return commandList != nullptr;
}

// Upload a 2D mip chain (layer 0) with one submit. The texture needs mipLevels >= mipCount,
// TransferDst usage and `format` as its format; it goes from Undefined to ShaderResource.
// To upload several textures with one submit, use StageTextureMips and SubmitStagedTextureUploads.
inline void UploadTextureMips(IDevice* device,
                              TextureHandle tex,
                              const TextureMipUploadEntry* mips,
                              uint32_t mipCount,
                              TextureFormat format,
                              const char* stagingDebugName = "TexMipUploadStaging")
{
    if (!device || !mips || mipCount == 0)
        return;
    std::optional<StagedTextureUpload> upload = StageTextureMips(*device, format, {mips, mipCount}, stagingDebugName);
    if (!upload)
        return;
    upload->Texture = tex;
    SubmitStagedTextureUploads(*device, {&*upload, 1});
}

// Upload tightly or explicitly pitched voxel data to a single existing 3D texture.
// Texture must already be created with depth > 1 and TextureUsage::TransferDst.
// Initial layout is assumed Undefined; final state is ShaderResource.
inline void UploadTexture3D(IDevice* device,
                            TextureHandle tex,
                            const void* pixels,
                            uint32_t width, uint32_t height, uint32_t depth,
                            size_t rowPitchBytes,
                            size_t slicePitchBytes = 0,
                            const char* stagingDebugName = "Tex3DUploadStaging")
{
    TextureUploadEntry entry{tex, pixels, width, height, depth, rowPitchBytes, slicePitchBytes};
    UploadTexturesBatched(device, &entry, 1, stagingDebugName);
}

} // namespace Rendering
} // namespace GameEngine
