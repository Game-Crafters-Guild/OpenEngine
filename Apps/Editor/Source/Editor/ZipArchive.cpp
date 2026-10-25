#include "Editor/ZipArchive.h"

#include "Logger/Logger.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
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

// An open archive reader; closes the reader and the file on destruction.
class ZipReader
{
  public:
    ZipReader() = default;
    ZipReader(const ZipReader&) = delete;
    ZipReader& operator=(const ZipReader&) = delete;

    ~ZipReader()
    {
        if (m_ReaderOpen)
            mz_zip_reader_end(&m_Archive);
        if (m_File)
            std::fclose(m_File);
    }

    bool Open(const fs::path& zipFile, std::string* outError)
    {
        m_File = OpenZipFile(zipFile);
        if (!m_File)
        {
            if (outError)
                *outError = "Could not open archive: " + zipFile.generic_string();
            return false;
        }
        m_ReaderOpen = mz_zip_reader_init_cfile(&m_Archive, m_File, 0, 0);
        if (!m_ReaderOpen)
        {
            if (outError)
                *outError = "Not a valid .zip archive.";
            return false;
        }
        return true;
    }

    mz_zip_archive* Archive() { return &m_Archive; }

  private:
    FILE* m_File = nullptr;
    mz_zip_archive m_Archive{};
    bool m_ReaderOpen = false;
};

// The safe file entries of an open archive, in archive order. Unsafe names are
// skipped with a warning; folder entries are skipped silently.
bool ReadFileEntries(mz_zip_archive* archive,
                     std::vector<std::pair<std::string, mz_uint>>& outEntries,
                     std::string* outError)
{
    const mz_uint fileCount = mz_zip_reader_get_num_files(archive);
    outEntries.reserve(fileCount);
    for (mz_uint index = 0; index < fileCount; ++index)
    {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(archive, index, &stat))
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
        outEntries.emplace_back(std::move(name), index);
    }
    return true;
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

bool CompareEntryNames(const std::pair<std::string, mz_uint>& entry, const std::string& name)
{
    return entry.first < name;
}

} // namespace

bool ListZipEntries(const fs::path& zipFile, std::vector<std::string>& outEntryNames,
                    std::string* outError)
{
    ZipReader reader;
    if (!reader.Open(zipFile, outError))
        return false;

    std::vector<std::pair<std::string, mz_uint>> entries;
    if (!ReadFileEntries(reader.Archive(), entries, outError))
        return false;

    outEntryNames.reserve(outEntryNames.size() + entries.size());
    for (auto& entry : entries)
        outEntryNames.push_back(std::move(entry.first));
    return true;
}

bool ExtractZipEntries(const fs::path& zipFile, std::span<const ZipEntryExtraction> entries,
                       std::vector<ZipEntryFailure>& outFailures, std::string* outError)
{
    ZipReader reader;
    if (!reader.Open(zipFile, outError))
        return false;

    // Sorted by name for lookup. A case-sensitive mz_zip_reader_locate_file is a
    // linear scan, so one lookup per requested entry would be quadratic.
    std::vector<std::pair<std::string, mz_uint>> files;
    if (!ReadFileEntries(reader.Archive(), files, outError))
        return false;
    std::sort(files.begin(), files.end());

    const size_t failureCountBefore = outFailures.size();
    for (const ZipEntryExtraction& entry : entries)
    {
        if (!IsSafeEntryName(entry.EntryName))
        {
            outFailures.push_back({entry.EntryName, "The entry name is not a safe relative path."});
            continue;
        }
        const auto found =
            std::lower_bound(files.begin(), files.end(), entry.EntryName, CompareEntryNames);
        if (found == files.end() || found->first != entry.EntryName)
        {
            outFailures.push_back({entry.EntryName, "The archive has no file with this name."});
            continue;
        }
        std::string message = ExtractEntry(reader.Archive(), found->second, entry.Destination);
        if (!message.empty())
            outFailures.push_back({entry.EntryName, std::move(message)});
    }
    return outFailures.size() == failureCountBefore;
}

bool ExtractZipArchive(const fs::path& zipFile, const fs::path& destination,
                       const std::string& innerRoot, std::string* outError)
{
    std::vector<std::string> names;
    if (!ListZipEntries(zipFile, names, outError))
        return false;

    const std::string prefix = innerRoot.empty() ? std::string{} : innerRoot + "/";
    std::vector<ZipEntryExtraction> extractions;
    extractions.reserve(names.size());
    for (std::string& name : names)
    {
        if (!name.starts_with(prefix))
            continue;
        const fs::path target = destination / fs::path(name.substr(prefix.size()));
        extractions.push_back({std::move(name), target});
    }

    if (extractions.empty())
    {
        if (outError)
            *outError = innerRoot.empty() ? "The archive is empty."
                                          : "The archive has no folder '" + innerRoot + "'.";
        return false;
    }

    std::vector<ZipEntryFailure> failures;
    if (ExtractZipEntries(zipFile, extractions, failures, outError))
        return true;
    if (!failures.empty() && outError)
        *outError = "Could not extract '" + failures.front().EntryName + "': " + failures.front().Message;
    return false;
}

} // namespace GameEngine::Editor
