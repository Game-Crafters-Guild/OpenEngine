#pragma once

#include "Types/FlatSet.h"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

namespace GameEngine::Rendering
{

struct ShaderStageRecord;

// Collects the stage records of development runs in a text file, one FormatShaderStageRecord line each, with no
// duplicates. Lines are only appended, under a file lock that all processes share, so an editor and a Player on
// one workspace keep each other's lines. Thread-safe: pipeline builds record from job workers.
class ShaderStageRecorder
{
public:
    // Adds @p record to @p file when the file does not hold it. An empty path records nothing.
    void Record(const std::filesystem::path& file, const ShaderStageRecord& record);

private:
    void ReadAppendedLinesLocked();

    std::mutex m_Mutex;
    std::filesystem::path m_File;
    FlatSet<std::string> m_Lines;
    // The bytes of m_File that m_Lines holds; other processes append after them.
    std::uintmax_t m_ReadBytes = 0;
};

} // namespace GameEngine::Rendering
