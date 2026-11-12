#include "ShaderStageRecorder.h"

#include "Rendering/ShaderCache/ShaderStageRecord.h"

#include "FileSystem/ScopedFileLock.h"
#include "Types/StringUtils.h"

#include <fstream>
#include <iterator>

namespace GameEngine::Rendering
{

void ShaderStageRecorder::Record(const std::filesystem::path& file, const ShaderStageRecord& record)
{
    if (file.empty())
        return;
    std::string line = FormatShaderStageRecord(record);
    std::lock_guard lock(m_Mutex);
    if (file != m_File)
    {
        m_File = file;
        m_Lines = {};
        m_ReadBytes = 0;
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
    }
    if (m_Lines.Contains(line))
        return;
    // A failed lock still appends: a line written twice is harmless, a lost line is not.
    const FileSystem::ScopedFileLock fileLock(std::filesystem::path(m_File) += ".lock");
    ReadAppendedLinesLocked();
    if (!m_Lines.Insert(line))
        return;
    std::error_code ec;
    // Bytes past the last line break are a line that a stopped run did not end; this line starts after it.
    const bool unendedLine = std::filesystem::file_size(m_File, ec) > m_ReadBytes && !ec;
    {
        std::ofstream out(m_File, std::ios::binary | std::ios::app);
        if (unendedLine)
            out << '\n';
        out << line << '\n';
    }
    m_ReadBytes = std::filesystem::file_size(m_File, ec);
}

void ShaderStageRecorder::ReadAppendedLinesLocked()
{
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(m_File, ec);
    if (ec || size < m_ReadBytes)
    {
        // The file is gone or was cut: record every line again.
        m_Lines = {};
        m_ReadBytes = 0;
    }
    if (ec || size == m_ReadBytes)
        return;
    std::ifstream in(m_File, std::ios::binary);
    in.seekg(static_cast<std::streamoff>(m_ReadBytes));
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    // Only whole lines count; a partial last line stays unread until it ends.
    const std::size_t end = text.rfind('\n');
    if (end == std::string::npos)
        return;
    ShaderStageRecord parsed;
    for (std::string_view line : Split(std::string_view(text).substr(0, end), '\n'))
    {
        if (ParseShaderStageRecord(line, parsed))
            m_Lines.Insert(std::string(line));
    }
    m_ReadBytes += end + 1;
}

} // namespace GameEngine::Rendering
