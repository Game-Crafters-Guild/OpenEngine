#include "Assets/SvgRasterizer.h"

#include "Logger/Logger.h"
#include "Platform/Capabilities.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <mutex>
#include <system_error>

#if defined(GE_HAVE_THORVG)
#include <thread>
#include <thorvg.h>
#endif

namespace GameEngine {

namespace
{
constexpr uint32 kMaxThorvgThreads = 4;

// Accessed from multiple threads (editor settings write, asset loaders read).
static std::atomic<float32> s_TextureDefaultSize{256.0f};

#if defined(GE_HAVE_THORVG)
static std::once_flag s_ThorvgInitFlag;
// Relaxed ordering is safe: std::call_once provides the happens-before
// guarantee that makes the store in the lambda visible to all readers.
static std::atomic<bool> s_ThorvgInitResult{false};

#if defined(TVG_VERSION_MAJOR) && TVG_VERSION_MAJOR >= 1
struct ThorvgPaintDeleter
{
    void operator()(tvg::Paint* paint) const noexcept
    {
        tvg::Paint::rel(paint);
    }
};

using ThorvgPicturePtr = std::unique_ptr<tvg::Picture, ThorvgPaintDeleter>;
using ThorvgCanvasPtr = std::unique_ptr<tvg::SwCanvas>;

static ThorvgPicturePtr MakeThorvgPicture()
{
    return ThorvgPicturePtr(tvg::Picture::gen());
}

static ThorvgCanvasPtr MakeThorvgCanvas()
{
    return ThorvgCanvasPtr(tvg::SwCanvas::gen());
}
#else
using ThorvgPicturePtr = std::unique_ptr<tvg::Picture>;
using ThorvgCanvasPtr = std::unique_ptr<tvg::SwCanvas>;

static ThorvgPicturePtr MakeThorvgPicture()
{
    return tvg::Picture::gen();
}

static ThorvgCanvasPtr MakeThorvgCanvas()
{
    return tvg::SwCanvas::gen();
}
#endif

static bool EnsureThorvgInitialized()
{
    std::call_once(s_ThorvgInitFlag, []()
    {
        // ThorVG's pool is additional to the engine's reserved workers.
        // Zero selects its synchronous rasterizer without creating pthreads.
        const uint32 threads = Platform::SupportsAuxiliaryThreadPools()
            ? std::min(kMaxThorvgThreads, std::max(1u, std::thread::hardware_concurrency()))
            : 0u;
#if defined(TVG_VERSION_MAJOR) && TVG_VERSION_MAJOR >= 1
        const tvg::Result r = tvg::Initializer::init(threads);
#else
        const tvg::Result r = tvg::Initializer::init(tvg::CanvasEngine::Sw, threads);
#endif
        if (r != tvg::Result::Success)
        {
            Logger::Log::Error("RasterizeSvgToRgbaAtSize: ThorVG Initializer::init failed with code {}", static_cast<int>(r));
            s_ThorvgInitResult.store(false, std::memory_order_relaxed);
            return;
        }
        s_ThorvgInitResult.store(true, std::memory_order_relaxed);
    });
    return s_ThorvgInitResult.load(std::memory_order_relaxed);
}
#endif
} // namespace

void SetSvgTextureRasterizerDefaultSize(float32 targetPixels)
{
    const float32 clamped = std::clamp(
        targetPixels, static_cast<float32>(kMinSvgRasterSize), static_cast<float32>(kMaxSvgRasterSize));
    s_TextureDefaultSize.store(clamped, std::memory_order_relaxed);
}

float32 GetSvgTextureRasterizerDefaultSize()
{
    return s_TextureDefaultSize.load(std::memory_order_relaxed);
}

bool ParseSvgRasterSizeMeta(const std::string& value, uint32& outTargetPixels)
{
    if (value.empty())
        return false;

    uint32 parsed = 0;
    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto result = std::from_chars(begin, end, parsed, 10);
    if (result.ec != std::errc{} || result.ptr != end)
        return false;

    outTargetPixels = std::clamp(parsed, kMinSvgRasterSize, kMaxSvgRasterSize);
    return true;
}

bool GetSvgSourceSize(const std::string& svgUtf8,
                      float32& outWidth,
                      float32& outHeight)
{
#if !defined(GE_HAVE_THORVG)
    (void)svgUtf8;
    (void)outWidth;
    (void)outHeight;
    return false;
#else
    if (svgUtf8.empty() || !EnsureThorvgInitialized())
        return false;

    ThorvgPicturePtr picture = MakeThorvgPicture();
    if (!picture)
        return false;

    const tvg::Result loadResult =
#if defined(TVG_VERSION_MAJOR) && TVG_VERSION_MAJOR >= 1
        picture->load(svgUtf8.c_str(), static_cast<uint32_t>(svgUtf8.size()), "svg", nullptr, true);
#else
        picture->load(svgUtf8.c_str(), static_cast<uint32_t>(svgUtf8.size()), "svg", true);
#endif
    if (loadResult != tvg::Result::Success)
        return false;

    float32 width = 0.0f;
    float32 height = 0.0f;
    picture->size(&width, &height);
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0.0f || height <= 0.0f)
        return false;

    outWidth = width;
    outHeight = height;
    return true;
#endif
}

bool CalculateSvgRasterDimensions(float32 sourceWidth,
                                  float32 sourceHeight,
                                  float32 targetPixels,
                                  uint32& outWidth,
                                  uint32& outHeight)
{
    if (!std::isfinite(sourceWidth) || !std::isfinite(sourceHeight) ||
        !std::isfinite(targetPixels) || sourceWidth <= 0.0f ||
        sourceHeight <= 0.0f || targetPixels <= 0.0f)
    {
        return false;
    }

    // Explicit callers such as platform icon creation may legitimately request
    // a sub-16-pixel target. Per-asset/editor settings clamp to 16 before here.
    const float32 targetPx = std::clamp(
        targetPixels, 1.0f, static_cast<float32>(kMaxSvgRasterSize));
    const float32 effectiveScale = targetPx / std::max(sourceWidth, sourceHeight);

    outWidth = static_cast<uint32>(std::max(0.0f, std::round(sourceWidth * effectiveScale)));
    outHeight = static_cast<uint32>(std::max(0.0f, std::round(sourceHeight * effectiveScale)));
    outWidth = std::clamp<uint32>(outWidth, 1u, kMaxSvgRasterSize);
    outHeight = std::clamp<uint32>(outHeight, 1u, kMaxSvgRasterSize);
    return true;
}

namespace
{
#if defined(GE_HAVE_THORVG)
// Parse `svgUtf8` into a ThorVG picture and report its source size. Logs the
// failure under `caller` and returns null.
ThorvgPicturePtr LoadSvgPicture(const std::string& svgUtf8, const char* caller, float& outWidth, float& outHeight)
{
    if (svgUtf8.empty())
    {
        Logger::Log::Error("{}: empty SVG string", caller);
        return nullptr;
    }
    if (!EnsureThorvgInitialized())
        return nullptr;
    ThorvgPicturePtr picture = MakeThorvgPicture();
    if (!picture)
    {
        Logger::Log::Error("{}: failed to create ThorVG picture", caller);
        return nullptr;
    }

    const tvg::Result loadResult =
#if defined(TVG_VERSION_MAJOR) && TVG_VERSION_MAJOR >= 1
        picture->load(svgUtf8.c_str(), static_cast<uint32_t>(svgUtf8.size()), "svg", nullptr, true);
#else
        picture->load(svgUtf8.c_str(), static_cast<uint32_t>(svgUtf8.size()), "svg", true);
#endif
    if (loadResult != tvg::Result::Success)
    {
        Logger::Log::Error("{}: ThorVG failed to load SVG, error code {}", caller, static_cast<int>(loadResult));
        return nullptr;
    }

    outWidth = 0.0f;
    outHeight = 0.0f;
    picture->size(&outWidth, &outHeight);
    if (outWidth <= 0.0f || outHeight <= 0.0f)
    {
        Logger::Log::Error("{}: SVG reported non-positive size {}x{}", caller, outWidth, outHeight);
        return nullptr;
    }
    return picture;
}

// Draw a loaded picture into a width x height RGBA8 buffer. The picture is
// sized to the whole canvas; the SVG's own preserveAspectRatio decides how a
// box of another aspect is filled.
bool RasterizeSvgPicture(ThorvgPicturePtr picture, uint32 width, uint32 height, const char* caller,
                         SvgRasterizedImage& outImage)
{
    picture->size(static_cast<float>(width), static_cast<float>(height));

    ThorvgCanvasPtr canvas = MakeThorvgCanvas();
    if (!canvas)
    {
        Logger::Log::Error("{}: failed to create ThorVG SwCanvas", caller);
        return false;
    }

    const size_t bufferSize = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    auto buffer = std::make_unique<uint8[]>(bufferSize);

    const tvg::Result targetResult =
        canvas->target(reinterpret_cast<uint32_t*>(buffer.get()),
                       width,
                       width,
                       height,
#if defined(TVG_VERSION_MAJOR) && TVG_VERSION_MAJOR >= 1
                       tvg::ColorSpace::ABGR8888S);
#else
                       tvg::SwCanvas::ABGR8888S);
#endif
    if (targetResult != tvg::Result::Success)
    {
        Logger::Log::Error("{}: ThorVG failed to set canvas target, error {}", caller, static_cast<int>(targetResult));
        return false;
    }

#if defined(TVG_VERSION_MAJOR) && TVG_VERSION_MAJOR >= 1
    const tvg::Result pushResult = canvas->add(picture.get());
    if (pushResult == tvg::Result::Success)
    {
        picture.release();
    }
#else
    const tvg::Result pushResult = canvas->push(std::move(picture));
#endif
    if (pushResult != tvg::Result::Success)
    {
        Logger::Log::Error("{}: ThorVG failed to add picture, error {}", caller, static_cast<int>(pushResult));
        return false;
    }

    const tvg::Result drawResult = canvas->draw();
    if (drawResult != tvg::Result::Success)
    {
        Logger::Log::Error("{}: ThorVG draw failed, error {}", caller, static_cast<int>(drawResult));
        return false;
    }

    const tvg::Result syncResult = canvas->sync();
    if (syncResult != tvg::Result::Success)
    {
        Logger::Log::Error("{}: ThorVG sync failed, error {}", caller, static_cast<int>(syncResult));
        return false;
    }

    outImage.Width = width;
    outImage.Height = height;
    outImage.Data = std::move(buffer);
    outImage.DataSize = bufferSize;
    return true;
}
#endif

#if defined(GE_HAVE_THORVG)
// The rasterizer's own anti-aliasing at icon sizes adds the coverage of
// overlapping strokes instead of uniting it (the crossings of a "#" draw
// full white where the union covers three quarters of the pixel) and errs by
// up to a quarter of full scale on single edges. Rendering at kSupersample
// times the size and box-filtering down brings the coverage to within a few
// levels of the exact area. Large rasters, where an edge pixel matters less,
// take a smaller factor so the fine raster stays within kMaxFineEdge.
constexpr uint32 kSupersample = 4;
constexpr uint32 kMaxFineEdge = 1024;

uint32 SupersampleFactor(uint32 width, uint32 height)
{
    const uint32 edge = std::max(width, height);
    return std::clamp(kMaxFineEdge / std::max(1u, edge), 1u, kSupersample);
}

// Average each factor x factor block of `fine` (straight alpha) into one
// pixel: colour weighted by alpha, so a transparent sub-pixel adds no colour.
void BoxFilterDown(const SvgRasterizedImage& fine, uint32 factor, uint32 width, uint32 height,
                   SvgRasterizedImage& outImage)
{
    const size_t size = static_cast<size_t>(width) * height * 4u;
    auto buffer = std::make_unique<uint8[]>(size);
    const uint32 samples = factor * factor;
    const uint8* src = fine.Data.get();
    for (uint32 y = 0; y < height; ++y)
    {
        for (uint32 x = 0; x < width; ++x)
        {
            uint32 alpha = 0;
            uint32 r = 0;
            uint32 g = 0;
            uint32 b = 0;
            for (uint32 sy = 0; sy < factor; ++sy)
            {
                const uint8* row = src + (static_cast<size_t>(y * factor + sy) * fine.Width + x * factor) * 4u;
                for (uint32 sx = 0; sx < factor; ++sx)
                {
                    const uint8* p = row + sx * 4u;
                    alpha += p[3];
                    r += p[0] * p[3];
                    g += p[1] * p[3];
                    b += p[2] * p[3];
                }
            }
            uint8* dst = buffer.get() + (static_cast<size_t>(y) * width + x) * 4u;
            dst[3] = static_cast<uint8>((alpha + samples / 2) / samples);
            dst[0] = alpha ? static_cast<uint8>((r + alpha / 2) / alpha) : 0;
            dst[1] = alpha ? static_cast<uint8>((g + alpha / 2) / alpha) : 0;
            dst[2] = alpha ? static_cast<uint8>((b + alpha / 2) / alpha) : 0;
        }
    }
    outImage.Width = width;
    outImage.Height = height;
    outImage.Data = std::move(buffer);
    outImage.DataSize = size;
}
#endif

void ClearRasterizedImage(SvgRasterizedImage& outImage)
{
    outImage.Width = 0;
    outImage.Height = 0;
    outImage.Data.reset();
    outImage.DataSize = 0;
}
} // namespace

bool RasterizeSvgToRgbaAtSize(const std::string& svgUtf8,
                              float32 targetPixels,
                              SvgRasterizedImage& outImage)
{
#if !defined(GE_HAVE_THORVG)
    Logger::Log::Warning("RasterizeSvgToRgbaAtSize: ThorVG not available at build time; SVGs will fall back to dummy textures");
    (void)svgUtf8;
    (void)targetPixels;
    ClearRasterizedImage(outImage);
    return false;
#else
    constexpr const char* kCaller = "RasterizeSvgToRgbaAtSize";
    float fw = 0.0f;
    float fh = 0.0f;
    ThorvgPicturePtr picture = LoadSvgPicture(svgUtf8, kCaller, fw, fh);
    if (!picture)
        return false;

    uint32 width = 0;
    uint32 height = 0;
    if (!CalculateSvgRasterDimensions(fw, fh, targetPixels, width, height))
    {
        Logger::Log::Error(
            "RasterizeSvgToRgbaAtSize: invalid source {}x{} or target size {}", fw, fh, targetPixels);
        return false;
    }
    return RasterizeSvgPicture(std::move(picture), width, height, kCaller, outImage);
#endif
}

bool RasterizeSvgToRgbaAtDimensions(const std::string& svgUtf8,
                                    uint32 width,
                                    uint32 height,
                                    SvgRasterizedImage& outImage)
{
#if !defined(GE_HAVE_THORVG)
    (void)svgUtf8;
    (void)width;
    (void)height;
    ClearRasterizedImage(outImage);
    return false;
#else
    constexpr const char* kCaller = "RasterizeSvgToRgbaAtDimensions";
    if (width == 0 || height == 0 || width > kMaxSvgRasterSize || height > kMaxSvgRasterSize)
    {
        Logger::Log::Error("{}: invalid raster size {}x{}", kCaller, width, height);
        return false;
    }
    float fw = 0.0f;
    float fh = 0.0f;
    ThorvgPicturePtr picture = LoadSvgPicture(svgUtf8, kCaller, fw, fh);
    if (!picture)
        return false;

    const uint32 factor = SupersampleFactor(width, height);
    if (factor == 1)
        return RasterizeSvgPicture(std::move(picture), width, height, kCaller, outImage);
    SvgRasterizedImage fine;
    if (!RasterizeSvgPicture(std::move(picture), width * factor, height * factor, kCaller, fine))
        return false;
    BoxFilterDown(fine, factor, width, height, outImage);
    return true;
#endif
}

} // namespace GameEngine
