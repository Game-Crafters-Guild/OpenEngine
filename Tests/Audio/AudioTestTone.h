#pragma once

// A clip synthesized in memory for the audio tests: decoded through the same
// miniaudio path production WAVs take, with no disk or asset registry.

#include "Types/Types.h"

#include <cmath>
#include <cstdint>

namespace AudioTestSupport
{

// A tiny valid 16-bit PCM WAV (0.5 s, 440 Hz, mono, 8 kHz) so the clip
// decodes through the same miniaudio path production WAVs take.
inline GameEngine::Vector<GameEngine::uint8> MakeTestToneWav()
{
    constexpr std::uint32_t kSampleRate = 8000;
    constexpr std::uint32_t kFrameCount = 4000;
    constexpr std::uint16_t kChannels = 1;
    constexpr std::uint16_t kBitsPerSample = 16;
    constexpr std::uint32_t kBytesPerFrame = kChannels * (kBitsPerSample / 8);
    constexpr std::uint32_t kDataBytes = kFrameCount * kBytesPerFrame;

    GameEngine::Vector<GameEngine::uint8> wav;
    wav.reserve(44 + kDataBytes);

    auto putU16 = [&](std::uint16_t v)
    {
        wav.push_back(static_cast<GameEngine::uint8>(v & 0xFF));
        wav.push_back(static_cast<GameEngine::uint8>((v >> 8) & 0xFF));
    };
    auto putU32 = [&](std::uint32_t v)
    {
        putU16(static_cast<std::uint16_t>(v & 0xFFFF));
        putU16(static_cast<std::uint16_t>(v >> 16));
    };
    auto putTag = [&](const char* tag) { wav.insert(wav.end(), tag, tag + 4); };

    putTag("RIFF");
    putU32(36 + kDataBytes);
    putTag("WAVE");
    putTag("fmt ");
    putU32(16);
    putU16(1); // PCM
    putU16(kChannels);
    putU32(kSampleRate);
    putU32(kSampleRate * kBytesPerFrame); // byte rate
    putU16(kBytesPerFrame);               // block align
    putU16(kBitsPerSample);
    putTag("data");
    putU32(kDataBytes);

    for (std::uint32_t i = 0; i < kFrameCount; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(kSampleRate);
        const float s = std::sin(2.0f * 3.14159265f * 440.0f * t);
        putU16(static_cast<std::uint16_t>(static_cast<std::int16_t>(s * 0.25f * 32767.0f)));
    }
    return wav;
}

} // namespace AudioTestSupport
