#include "PageCodec.h"

#include "HeightPageCodec.h"
#include "Bc5PageCodec.h"
#include "BytePageCodec.h"

namespace GameEngine::PageStreaming
{

std::unique_ptr<PageCodec> MakePageCodec(const PageStoreHeader& header)
{
    switch (header.Format)
    {
    case PageFieldFormat::HeightSteps:
        return std::make_unique<HeightPageCodec>(header);
    case PageFieldFormat::Rgba8:
    case PageFieldFormat::Rgba8Srgb:
    case PageFieldFormat::Rg8:
        return std::make_unique<BytePageCodec>();
    case PageFieldFormat::Bc5:
        return std::make_unique<Bc5PageCodec>();
    }
    return nullptr;
}

} // namespace GameEngine::PageStreaming
