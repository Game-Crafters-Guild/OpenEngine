#include "PlayerRunOptions.h"

#include "Logger/Logger.h"

#include <cstdlib>
#include <string_view>

namespace GameEngine
{

namespace
{
bool HasFlag(int argc, const char* const* argv, std::string_view flag)
{
    for (int i = 1; i < argc; ++i)
    {
        if (argv[i] == flag)
            return true;
    }
    return false;
}

// The value after `flag`, or nullptr when the flag is absent; `present` says which.
const char* ValueAfter(int argc, const char* const* argv, std::string_view flag, bool& present)
{
    present = false;
    for (int i = 1; i < argc; ++i)
    {
        if (argv[i] != flag)
            continue;
        present = true;
        return i + 1 < argc ? argv[i + 1] : nullptr;
    }
    return nullptr;
}

// --silent-audio, else GE_AUDIO_OUTPUT, else the device.
Audio::AudioOutput ResolveAudioOutput(int argc, const char* const* argv, const char* environment)
{
    if (HasFlag(argc, argv, "--silent-audio"))
        return Audio::AudioOutput::Silent;
    if (!environment || environment[0] == '\0')
        return Audio::AudioOutput::Device;
    if (const std::optional<Audio::AudioOutput> output = Audio::TryParseAudioOutput(environment))
        return *output;
    Logger::Log::Warning("Player: GE_AUDIO_OUTPUT='{}' is not an audio output; ignoring it and playing on the "
                         "device. Recognized values: silent, device.",
                         environment);
    return Audio::AudioOutput::Device;
}

// A headless run has no render device, so these flags ask for output it cannot make.
constexpr const char* kDeviceOnlyFlags[] = {"--write-movie",   "--screenshot",    "--hdr",
                                            "--hdr-mode",      "--hdr-bit-depth", "--hdr-swapchain-bit-depth",
                                            "--allow-hdr-toggle"};
} // namespace

bool TryParsePlayerDouble(const char* text, double& out)
{
    if (!text)
        return false;
    char* end = nullptr;
    const double value = std::strtod(text, &end);
    if (end == text || (end && *end != '\0'))
        return false;
    out = value;
    return true;
}

bool TryParsePlayerUint64(const char* text, uint64_t& out)
{
    if (!text || text[0] == '-')
        return false;
    char* end = nullptr;
    const auto value = std::strtoull(text, &end, 10);
    if (end == text || (end && *end != '\0'))
        return false;
    out = static_cast<uint64_t>(value);
    return true;
}

std::optional<PlayerRunOptions> ParsePlayerRunOptions(int argc, const char* const* argv,
                                                      const char* audioOutputEnvironment)
{
    PlayerRunOptions options;
    options.Headless = HasFlag(argc, argv, "--headless");
    options.AudioOutput = ResolveAudioOutput(argc, argv, audioOutputEnvironment);

    bool present = false;
    const char* quitAfter = ValueAfter(argc, argv, "--quit-after", present);
    if (present && !TryParsePlayerUint64(quitAfter, options.QuitAfterFrames))
    {
        Logger::Log::Error("Player: --quit-after requires a non-negative frame count, for example --quit-after 600");
        return std::nullopt;
    }

    const char* fixedFps = ValueAfter(argc, argv, "--fixed-fps", present);
    if (!options.Headless)
    {
        // Movie capture reads --fixed-fps as its frame rate; anything else has no fixed step.
        if (present && !HasFlag(argc, argv, "--write-movie"))
            Logger::Log::Warning("Player: --fixed-fps applies only with --headless or --write-movie; ignoring it");
        return options;
    }

    if (present && (!TryParsePlayerDouble(fixedFps, options.HeadlessFps) || options.HeadlessFps <= 0.0))
    {
        Logger::Log::Error("Player: --fixed-fps requires a positive numeric rate, for example --fixed-fps 60");
        return std::nullopt;
    }
    for (const char* flag : kDeviceOnlyFlags)
    {
        if (!HasFlag(argc, argv, flag))
            continue;
        Logger::Log::Error("Player: --headless runs without a render device, so {} cannot work. Remove {} "
                           "or run without --headless.",
                           flag, flag);
        return std::nullopt;
    }
    return options;
}

} // namespace GameEngine
