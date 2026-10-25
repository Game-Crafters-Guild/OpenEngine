#include "Editor/ZipArchiveReader.h"

#include "Logger/Logger.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <numeric>
#include <string_view>
#include <system_error>
#include <utility>

#include <miniz.h>

namespace GameEngine::Editor
{

namespace fs = std::filesystem;

namespace
{

// Zip entry names use '/' separators. Reject anything that could escape the
// destination (zip-slip) or that this extractor doesn't model.
bool IsSafeEntryName(const std::string& name)
{
    if (name.empty() || name.front() == '/' ||
        name.find('\\') != std::string::npos || name.find(':') != std::string::npos)
        return false;
    for (const std::string_view segment : Split(name, '/'))
    {
        if (segment == ".." || segment == ".")
            return false;
    }
    return true;
}

FILE* OpenZipFile(const fs::path& zipFile)
{
#if defined(_WIN32)
    // fopen takes ACP-encoded chars on Windows; go wide so non-ASCII user
    // paths (cache lives under the user profile) open correctly.
    return _wfopen(zipFile.c_str(), L"rb");
#else
    return std::fopen(zipFile.c_str(), "rb");
#endif
}

size_t WriteToStream(void* stream, mz_uint64 /*fileOffset*/, const void* data, size_t size)
{
    // miniz delivers an entry's bytes in order, so the offset always equals the
    // bytes written so far.
    auto* out = static_cast<std::ofstream*>(stream);
    out->write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return out->good() ? size : 0;
}

// Decompresses one entry into destination. On failure, returns the reason and
// removes any partial file.
std::string ExtractEntry(mz_zip_archive* archive, mz_uint index, const fs::path& destination)
{
    std::error_code ec;
    fs::create_directories(destination.parent_path(), ec);
    std::ofstream out(destination, std::ios::binary | std::ios::trunc);
    if (!out)
        return "Could not write '" + destination.generic_string() + "'.";

    const bool extracted = mz_zip_reader_extract_to_callback(archive, index, WriteToStream, &out, 0);
    out.close();
    if (extracted && !out.fail())
        return {};

    fs::remove(destination, ec);
    if (mz_zip_get_last_error(archive) == MZ_ZIP_WRITE_CALLBACK_FAILED || out.fail())
        return "Could not write '" + destination.generic_string() + "'.";
    return "Could not decompress the entry; the archive may be corrupt.";
}

} // namespace

// Heap-allocated and never moved: miniz keeps a pointer to Archive inside it.
struct ZipArchiveReader::State
{
    State() = default;
    State(const State&) = delete;
    State& operator=(const State&) = delete;

    ~State()
    {
        if (ReaderOpen)
            mz_zip_reader_end(&Archive);
        if (File)
            std::fclose(File);
    }

    // The safe file entries, in archive order: names and their miniz indices.
    // Unsafe names are skipped with a warning; folder entries are skipped silently.
    bool ReadFileTable(std::string* outError)
    {
        const mz_uint fileCount = mz_zip_reader_get_num_files(&Archive);
        Names.reserve(fileCount);
        Indices.reserve(fileCount);
        for (mz_uint index = 0; index < fileCount; ++index)
        {
            mz_zip_archive_file_stat stat{};
            if (!mz_zip_reader_file_stat(&Archive, index, &stat))
            {
                if (outError)
                    *outError = "Could not read the archive's file table.";
                return false;
            }
            if (stat.m_is_directory)
                continue;
            std::string name = stat.m_filename;
            if (!IsSafeEntryName(name))
            {
                Logger::Log::Warning("Zip archive entry '{}' has an unsafe name and is skipped", name);
                continue;
            }
            Names.push_back(std::move(name));
            Indices.push_back(index);
        }

        // A case-sensitive mz_zip_reader_locate_file is a linear scan, so one
        // lookup per requested entry would be quadratic; look names up here instead.
        ByName.resize(Names.size());
        std::iota(ByName.begin(), ByName.end(), size_t{0});
        std::sort(ByName.begin(), ByName.end(),
                  [this](size_t left, size_t right) { return Names[left] < Names[right]; });
        return true;
    }

    // The miniz index of the file named name, or nothing. Folder entries and
    // unsafe names are never found.
    std::optional<mz_uint> FindFile(const std::string& name) const
    {
        const auto found = std::lower_bound(
            ByName.begin(), ByName.end(), name,
            [this](size_t position, const std::string& key) { return Names[position] < key; });
        if (found == ByName.end() || Names[*found] != name)
            return std::nullopt;
        return Indices[*found];
    }

    FILE* File = nullptr;
    mz_zip_archive Archive{};
    bool ReaderOpen = false;
    std::vector<std::string> Names;
    // The miniz index of each name, parallel to Names.
    std::vector<mz_uint> Indices;
    // Positions in Names, sorted by name.
    std::vector<size_t> ByName;
};

std::optional<ZipArchiveReader> ZipArchiveReader::OpenFile(const fs::path& zipFile,
                                                           std::string* outError)
{
    auto state = std::make_unique<State>();
    state->File = OpenZipFile(zipFile);
    if (!state->File)
    {
        if (outError)
            *outError = "Could not open archive: " + zipFile.generic_string();
        return std::nullopt;
    }
    state->ReaderOpen = mz_zip_reader_init_cfile(&state->Archive, state->File, 0, 0);
    if (!state->ReaderOpen)
    {
        if (outError)
            *outError = "Not a valid .zip archive.";
        return std::nullopt;
    }
    if (!state->ReadFileTable(outError))
        return std::nullopt;
    return ZipArchiveReader(std::move(state));
}

std::optional<ZipArchiveReader> ZipArchiveReader::OpenMemory(std::span<const std::byte> archive,
                                                             std::string* outError)
{
    auto state = std::make_unique<State>();
    state->ReaderOpen = mz_zip_reader_init_mem(&state->Archive, archive.data(), archive.size(), 0);
    if (!state->ReaderOpen)
    {
        if (outError)
            *outError = "Not a valid .zip archive.";
        return std::nullopt;
    }
    if (!state->ReadFileTable(outError))
        return std::nullopt;
    return ZipArchiveReader(std::move(state));
}

ZipArchiveReader::ZipArchiveReader(std::unique_ptr<State> state) : m_State(std::move(state)) {}

ZipArchiveReader::ZipArchiveReader(ZipArchiveReader&& other) noexcept = default;
ZipArchiveReader& ZipArchiveReader::operator=(ZipArchiveReader&& other) noexcept = default;
ZipArchiveReader::~ZipArchiveReader() = default;

const std::vector<std::string>& ZipArchiveReader::EntryNames() const
{
    return m_State->Names;
}

bool ZipArchiveReader::ExtractEntries(std::span<const ZipEntryExtraction> entries,
                                      std::vector<ZipEntryFailure>& outFailures)
{
    const size_t failureCountBefore = outFailures.size();
    for (const ZipEntryExtraction& entry : entries)
    {
        if (!IsSafeEntryName(entry.EntryName))
        {
            outFailures.push_back({entry.EntryName, "The entry name is not a safe relative path."});
            continue;
        }
        const std::optional<mz_uint> index = m_State->FindFile(entry.EntryName);
        if (!index)
        {
            outFailures.push_back({entry.EntryName, "The archive has no file with this name."});
            continue;
        }
        std::string message = ExtractEntry(&m_State->Archive, *index, entry.Destination);
        if (!message.empty())
            outFailures.push_back({entry.EntryName, std::move(message)});
    }
    return outFailures.size() == failureCountBefore;
}

bool ZipArchiveReader::ReadEntry(const std::string& entryName, std::vector<std::byte>& outBytes,
                                 size_t maxBytes, std::string* outError)
{
    outBytes.clear();
    const std::optional<mz_uint> index = m_State->FindFile(entryName);
    if (!index)
    {
        if (outError)
            *outError = "The archive has no file named '" + entryName + "'.";
        return false;
    }

    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&m_State->Archive, *index, &stat))
    {
        if (outError)
            *outError = "Could not read the archive's file table.";
        return false;
    }
    if (stat.m_uncomp_size > maxBytes)
    {
        if (outError)
            *outError = "'" + entryName + "' is " + FormatGroupedInteger(stat.m_uncomp_size) +
                        " bytes, more than the " + FormatGroupedInteger(maxBytes) + "-byte limit.";
        return false;
    }

    // miniz stops decompressing at the buffer's end and checks the CRC, so an
    // entry cannot write past its stated size.
    outBytes.resize(static_cast<size_t>(stat.m_uncomp_size));
    if (!mz_zip_reader_extract_to_mem(&m_State->Archive, *index, outBytes.data(), outBytes.size(), 0))
    {
        outBytes.clear();
        if (outError)
            *outError = "Could not decompress '" + entryName + "'; the archive may be corrupt.";
        return false;
    }
    return true;
}

} // namespace GameEngine::Editor
