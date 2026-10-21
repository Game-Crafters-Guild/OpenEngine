// Diagnostic executable, not a test: the wall time of the texture cook's encode of one source
// image's whole mip chain (CompressTextureCookLevel per level, the production band encode) on N
// encode threads, and a dump of the levels and payloads for quality scoring. Numbers mean something
// only from a Release build on a quiet machine; the unit suites carry no timing thresholds.
//
//   TextureCookBenchmark --src <image> [--usage color|packed|normal|mask] [--srgb 0|1] [--threads N]
//                        [--runs N] [--dump <dir>] [--label <name>]
//   TextureCookBenchmark decode <bc7|bc5|bc4> <width> <height> <payload> <out.rgba>
//
// Output: one LEVEL line per level and run (label, run, level, width, height, encode ms) and one
// CHAIN line per run (decode, mip and encode milliseconds, wall milliseconds). N encode threads are
// the calling thread plus N-1 TextureCookWorkers helpers on a pool of N-1 workers, the decode
// gate's texture share sized to N-1, as the editor spreads a cook.

#include "Assets/AssetDecodeGate.h"
#include "Assets/TextureCook.h"
#include "Assets/TextureCookWorkers.h"
#include "Image/Decode.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace GameEngine;

namespace
{
struct Options
{
    std::string Source;
    std::string Usage = "color";
    bool Srgb = true;
    uint32 Threads = 1;
    uint32 Runs = 3;
    std::string Dump;
    std::string Label;
};

using Clock = std::chrono::steady_clock;

double MillisecondsSince(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

TextureCookOutput OutputFor(const std::string& usage)
{
    if (usage == "normal")
        return TextureCookOutput::BC5;
    if (usage == "mask")
        return TextureCookOutput::BC4;
    return TextureCookOutput::BC7;
}

TextureCookUsage UsageFor(const std::string& usage)
{
    if (usage == "normal")
        return TextureCookUsage::Normal;
    if (usage == "mask")
        return TextureCookUsage::Mask;
    if (usage == "packed")
        return TextureCookUsage::Packed;
    return TextureCookUsage::Color;
}

std::vector<uint8> ReadAll(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void WriteAll(const std::filesystem::path& path, const std::vector<uint8>& bytes)
{
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

struct EncodeThreads
{
    std::unique_ptr<JobSystem::WorkStealingThreadPool> Pool;
    std::unique_ptr<TextureCookWorkers> Workers;

    explicit EncodeThreads(uint32 threads)
    {
        if (threads <= 1)
            return;
        Pool = std::make_unique<JobSystem::WorkStealingThreadPool>(threads - 1);
        Workers = std::make_unique<TextureCookWorkers>(
            *Pool, std::make_shared<AssetDecodeGate>(threads - 1, threads + 1, [] {}));
    }
};

void DumpLevel(const std::string& dumpDir, size_t level, const TextureCookMipLevel& mip,
               const std::vector<uint8>& payload)
{
    const std::filesystem::path dir(dumpDir);
    std::filesystem::create_directories(dir);
    const std::string stem =
        "level" + std::to_string(level) + "_" + std::to_string(mip.Width) + "x" + std::to_string(mip.Height);
    WriteAll(dir / (stem + ".rgba"), mip.Bytes);
    WriteAll(dir / (stem + ".bc"), payload);
}

int RunEncode(const Options& options)
{
    const std::vector<uint8> sourceBytes = ReadAll(options.Source);
    if (sourceBytes.empty())
    {
        std::fprintf(stderr, "cannot read %s\n", options.Source.c_str());
        return 2;
    }
    const TextureCookOutput output = OutputFor(options.Usage);
    TextureCookSettings settings;
    settings.Usage = UsageFor(options.Usage);
    EncodeThreads threads(options.Threads);

    for (uint32 run = 0; run < options.Runs; ++run)
    {
        const auto start = Clock::now();
        Image::Image8 decoded = Image::Decode8(sourceBytes, options.Source, Image::DecodeChannels::Rgba);
        if (!decoded.Valid)
            return 3;
        std::vector<TextureCookMipLevel> chain(1);
        chain[0].Width = decoded.Width;
        chain[0].Height = decoded.Height;
        chain[0].Bytes = std::move(decoded.Pixels);
        const double decodeMs = MillisecondsSince(start);

        const auto mipStart = Clock::now();
        std::string error;
        if (!BuildTextureCookMipChain(chain, ComputeTextureCookMipCount(chain[0].Width, chain[0].Height, settings),
                                      /*isFloat=*/false, options.Srgb, settings, &error))
        {
            std::fprintf(stderr, "mip chain: %s\n", error.c_str());
            return 4;
        }
        const double mipMs = MillisecondsSince(mipStart);

        double encodeMs = 0.0;
        for (size_t level = 0; level < chain.size(); ++level)
        {
            std::vector<uint8> payload;
            const auto levelStart = Clock::now();
            if (!CompressTextureCookLevel(chain[level], /*isFloat=*/false, output, /*bandBlocks=*/0, payload, error,
                                          {}, threads.Workers.get()))
            {
                std::fprintf(stderr, "encode: %s\n", error.c_str());
                return 5;
            }
            const double levelMs = MillisecondsSince(levelStart);
            encodeMs += levelMs;
            std::printf("LEVEL,%s,%u,%zu,%u,%u,%.3f\n", options.Label.c_str(), run, level, chain[level].Width,
                        chain[level].Height, levelMs);
            if (!options.Dump.empty() && run == 0)
                DumpLevel(options.Dump, level, chain[level], payload);
        }
        std::printf("CHAIN,%s,%u,decode_ms=%.1f,mips_ms=%.1f,encode_ms=%.1f,wall_ms=%.1f,levels=%zu\n",
                    options.Label.c_str(), run, decodeMs, mipMs, encodeMs, MillisecondsSince(start), chain.size());
        std::fflush(stdout);
    }
    return 0;
}

// Decodes a dumped payload with the engine's decoder (DirectXTex), for scoring against the level.
int RunDecode(const std::string& format, uint32 width, uint32 height, const std::string& in, const std::string& out)
{
    const TextureFormat textureFormat = format == "bc5"   ? TextureFormat::BC5
                                        : format == "bc4" ? TextureFormat::BC4
                                                          : TextureFormat::BC7;
    const std::vector<uint8> payload = ReadAll(in);
    std::vector<uint8> rgba;
    if (!DecodeBlockPayloadRGBA8(textureFormat, width, height, payload.data(), payload.size(), rgba))
        return 7;
    WriteAll(out, rgba);
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc == 7 && std::strcmp(argv[1], "decode") == 0)
        return RunDecode(argv[2], static_cast<uint32>(std::stoul(argv[3])), static_cast<uint32>(std::stoul(argv[4])),
                         argv[5], argv[6]);
    Options options;
    for (int i = 1; i + 1 < argc; i += 2)
    {
        const std::string key = argv[i];
        const std::string value = argv[i + 1];
        if (key == "--src") options.Source = value;
        else if (key == "--usage") options.Usage = value;
        else if (key == "--srgb") options.Srgb = value == "1";
        else if (key == "--threads") options.Threads = static_cast<uint32>(std::stoul(value));
        else if (key == "--runs") options.Runs = static_cast<uint32>(std::stoul(value));
        else if (key == "--dump") options.Dump = value;
        else if (key == "--label") options.Label = value;
    }
    if (!IsTextureCookEncoderAvailable())
    {
        std::fprintf(stderr, "no BC encoder compiled in\n");
        return 1;
    }
    return RunEncode(options);
}
