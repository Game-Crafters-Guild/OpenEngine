#pragma once

#include "PageStreaming/PageStoreFormat.h"

#include <atomic>
#include <functional>

namespace GameEngine::PageStreaming
{

class HeightSourceRows;
class PagePyramidBuilder;

// Level-0 rows read per step of the cook: one page row's owned rows.
constexpr uint32 kCookBandRows = kPageOwnedSamples;

// True when `cancel` is set and holds true.
inline bool Cancelled(const std::atomic<bool>* cancel)
{
    return cancel && cancel->load();
}

enum class CookBandResult : uint8
{
    Done,
    ReadFailed,
    Cancelled
};

// Pushes source rows [firstRow, firstRow + rowCount) through `builder` a band at a time, and reads
// the next band while one builds. After each push, `afterBand` gets the band, its first row from
// firstRow and its row count. The reader has no read outstanding when this returns.
CookBandResult PushCookBands(HeightSourceRows& rows, uint32 firstRow, uint32 rowCount, uint32 samplesX,
                             const std::atomic<bool>* cancel, PagePyramidBuilder& builder,
                             const std::function<void(const float32* band, uint32 row, uint32 count)>& afterBand);

} // namespace GameEngine::PageStreaming
