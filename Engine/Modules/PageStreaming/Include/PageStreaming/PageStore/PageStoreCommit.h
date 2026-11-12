#pragma once

#include "PageStreaming/PageSampleRect.h"
#include "Types/Types.h"

namespace GameEngine::PageStreaming
{

/// The pages one PageStoreAccess::Commit makes readable again: the pages of level `Level` and coarser
/// that read level-0 samples `Level0Rect` (PageReadsLevel0Rect). A finer page is not touched.
struct PageStoreCommit
{
    PageSampleRect Level0Rect;
    uint32 Level = 0;
};

} // namespace GameEngine::PageStreaming
