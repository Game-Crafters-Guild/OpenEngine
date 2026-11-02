#include "Video/VideoWriter.h"

#include "Types/PathUtils.h"
#include "Types/StringUtils.h"

#include <filesystem>

namespace GameEngine::Video
{

VideoCodec GuessCodecForMoviePath(const std::string& path)
{
    const std::string extension = LowerCaseExtension(std::filesystem::path(path));
    if (extension == ".hevc" || extension == ".h265")
        return VideoCodec::HEVC;
    if (extension == ".mov")
        return VideoCodec::ProRes422;
    return VideoCodec::H264;
}

VideoCodec ParseVideoCodecName(const std::string& name, VideoCodec fallback)
{
    const std::string lowerName = ToLowerAscii(name);
    if (lowerName == "auto")
        return VideoCodec::Auto;
    if (lowerName == "h264" || lowerName == "avc")
        return VideoCodec::H264;
    if (lowerName == "h265" || lowerName == "hevc")
        return VideoCodec::HEVC;
    if (lowerName == "prores" || lowerName == "prores422" || lowerName == "prores_422")
        return VideoCodec::ProRes422;
    if (lowerName == "prores4444" || lowerName == "prores_4444")
        return VideoCodec::ProRes4444;
    return fallback;
}

const char* ToString(VideoCodec codec)
{
    switch (codec)
    {
    case VideoCodec::Auto: return "auto";
    case VideoCodec::H264: return "h264";
    case VideoCodec::HEVC: return "h265";
    case VideoCodec::ProRes422: return "prores422";
    case VideoCodec::ProRes4444: return "prores4444";
    }
    return "unknown";
}

} // namespace GameEngine::Video
