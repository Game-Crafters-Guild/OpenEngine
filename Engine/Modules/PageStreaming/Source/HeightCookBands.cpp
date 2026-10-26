#include "HeightCookBands.h"

#include "HeightSourceRows.h"
#include "PagePyramidBuilder.h"

#include <algorithm>
#include <vector>

namespace GameEngine::PageStreaming
{

CookBandResult PushCookBands(HeightSourceRows& rows, uint32 firstRow, uint32 rowCount, uint32 samplesX,
                             const std::atomic<bool>* cancel, PagePyramidBuilder& builder,
                             const std::function<void(const float32* band, uint32 row, uint32 count)>& afterBand)
{
    std::vector<float32> band(static_cast<std::size_t>(samplesX) * kCookBandRows);
    rows.RequestRows(firstRow, std::min(kCookBandRows, rowCount));
    for (uint32 row = 0; row < rowCount;)
    {
        const uint32 count = std::min(kCookBandRows, rowCount - row);
        if (!rows.TakeRows(band.data()))
            return CookBandResult::ReadFailed;
        const uint32 nextRow = row + count;
        if (nextRow < rowCount)
            rows.RequestRows(firstRow + nextRow, std::min(kCookBandRows, rowCount - nextRow));
        if (Cancelled(cancel))
        {
            if (nextRow < rowCount)
                rows.TakeRows(band.data()); // never leave a read outstanding past the reader
            return CookBandResult::Cancelled;
        }
        builder.PushLevel0Rows(reinterpret_cast<const uint8*>(band.data()), count);
        if (afterBand)
            afterBand(band.data(), row, count);
        row = nextRow;
    }
    return CookBandResult::Done;
}

} // namespace GameEngine::PageStreaming
