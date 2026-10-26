#include "Editor/ZipArchiveReader.h"

#include "AssetCore/PathNormalization.h"
#include "Logger/Logger.h"
#include "Types/PathUtils.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <limits>
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

// The bytes of a local file header before the entry's name, and the offsets of its name and
// extra-field lengths (PKWARE APPNOTE.TXT 4.3.7).
constexpr size_t kLocalHeaderSize = 30;
constexpr size_t kLocalNameLengthOffset = 26;
constexpr size_t kLocalExtraLengthOffset = 28;

// The names Windows opens as a device in any folder, in any case and with any extension:
// these six, and COM or LPT followed by one digit or by a superscript 1, 2 or 3.
constexpr std::array<std::string_view, 6> kDeviceNames = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"};
constexpr std::array<std::string_view, 2> kNumberedDeviceNames = {"COM", "LPT"};
constexpr std::array<std::string_view, 3> kSuperscriptDigits = {"\xC2\xB9", "\xC2\xB2", "\xC2\xB3"};

// The compression methods the reader decompresses, and the general-purpose flag of patch data,
// which it does not (PKWARE APPNOTE.TXT 4.4.4 and 4.4.5).
constexpr mz_uint16 kStoredMethod = 0;
constexpr mz_uint16 kPatchDataFlag = 1 << 5;

// miniz's sorted central directory serves only its case-insensitive name lookup, which the
// reader does not use.
constexpr mz_uint kReaderFlags = MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY;

// An entry is written to its destination with this suffix, then renamed when it is whole.
constexpr std::string_view kPartialFileSuffix = ".partial";

// A file entry of the archive's file table, before the entries that cannot be extracted are
// left out.
struct FileTableEntry
{
    std::string Name;
    mz_uint Index = 0;
    // The size the entry declares; miniz fails an entry whose data decompresses to more.
    mz_uint64 Size = 0;
    // The archive bytes the entry's local header, name, extra field and data start at and end
    // before.
    mz_uint64 DataBegin = 0;
    mz_uint64 DataEnd = 0;
    bool LeftOut = false;
};

// True when Windows opens segment, a file or folder name, as a device.
bool IsDeviceName(std::string_view segment)
{
    if (segment.empty())
        return false;
    // The stem is the name before its first '.', or the whole name; a device name keeps its
    // meaning with any extension.
    std::string_view stem = Split(segment, '.').front();
    while (!stem.empty() && stem.back() == ' ')
        stem.remove_suffix(1);
    for (const std::string_view device : kDeviceNames)
    {
        if (EqualsIgnoreCase(stem, device))
            return true;
    }
    for (const std::string_view device : kNumberedDeviceNames)
    {
        if (!StartsWithIgnoreCase(stem, device))
            continue;
        const std::string_view number = stem.substr(device.size());
        if (number.size() == 1 && std::isdigit(static_cast<unsigned char>(number.front())))
            return true;
        if (std::find(kSuperscriptDigits.begin(), kSuperscriptDigits.end(), number) != kSuperscriptDigits.end())
            return true;
    }
    return false;
}

// Zip entry names use '/' separators. Reject a name that is not UTF-8, that could escape the
// destination (zip-slip), that Windows renames or opens as a device, or that this extractor does
// not model. A segment that ends in '.' covers "." and "..".
bool IsSafeEntryName(const std::string& name)
{
    if (name.empty() || !IsValidUtf8(name) || name.front() == '/' || name.find('\\') != std::string::npos ||
        name.find(':') != std::string::npos || name.find('\0') != std::string::npos)
        return false;
    for (const std::string_view segment : Split(name, '/'))
    {
        if (segment.empty())
            continue;
        if (segment.back() == '.' || segment.back() == ' ' || IsDeviceName(segment))
            return false;
    }
    return true;
}

// The message of an entry whose file could not be written, with the reason. A reason can end
// in a period already, as Windows' system messages do.
std::string WriteFailure(const fs::path& destination, const std::error_code& reason)
{
    std::string message = "Could not write '" + PathToUtf8(destination) + "': " + reason.message();
    if (!message.ends_with('.'))
        message += '.';
    return message;
}

// The message of each entry of an ExtractEntries call whose entries together declare more than
// its limit.
std::string LimitFailure(std::uint64_t totalBytes, std::uint64_t maxTotalBytes)
{
    return "The chosen entries hold " + FormatMebibytes(totalBytes) + ", more than the limit of " +
           FormatMebibytes(maxTotalBytes) + " for one extraction, so none was written.";
}

// The two-byte little-endian number at header[at].
mz_uint64 ReadUint16(const std::array<unsigned char, kLocalHeaderSize>& header, size_t at)
{
    return mz_uint64{header[at]} | (mz_uint64{header[at + 1]} << 8);
}

// True when the reader can decompress the entry: stored or deflated, and not patch data.
// miniz's m_is_supported is also false for an encrypted entry, which the reader tells apart.
bool UsesSupportedCompression(const mz_zip_archive_file_stat& stat)
{
    return (stat.m_method == kStoredMethod || stat.m_method == MZ_DEFLATED) && (stat.m_bit_flag & kPatchDataFlag) == 0;
}

// The reason the last C library call failed, or an input/output error when it set none.
std::error_code LastSystemError()
{
    if (errno == 0)
        return std::make_error_code(std::errc::io_error);
    return {errno, std::generic_category()};
}

FILE* OpenZipFile(const fs::path& zipFile)
{
#if defined(_WIN32)
    // fopen reads the path in the active code page on Windows; the wide call opens a
    // non-ASCII path.
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

// The entry's whole name. miniz's file stat holds only the first 511 bytes of a name, and a
// name can be 65535 bytes long.
std::optional<std::string> ReadEntryName(mz_zip_archive* archive, mz_uint index)
{
    const mz_uint sizeWithNull = mz_zip_reader_get_filename(archive, index, nullptr, 0);
    if (sizeWithNull == 0)
        return std::nullopt;
    std::string name(sizeWithNull, '\0');
    mz_zip_reader_get_filename(archive, index, name.data(), sizeWithNull);
    name.resize(sizeWithNull - 1);
    return name;
}

// Leaves out every entry whose bytes overlap another entry's. One entry writes at most its
// declared size, which deflate bounds at about 1032 times its compressed bytes; a zip bomb
// goes past that by giving many entries the same compressed data.
void LeaveOutOverlappingEntries(std::vector<FileTableEntry>& entries)
{
    std::vector<size_t> byBegin(entries.size());
    std::iota(byBegin.begin(), byBegin.end(), size_t{0});
    std::sort(byBegin.begin(), byBegin.end(), [&entries](size_t left, size_t right) {
        return entries[left].DataBegin < entries[right].DataBegin;
    });
    // The entry, of those before, whose bytes end furthest into the archive.
    size_t furthest = 0;
    for (size_t at = 1; at < byBegin.size(); ++at)
    {
        FileTableEntry& entry = entries[byBegin[at]];
        FileTableEntry& before = entries[byBegin[furthest]];
        if (entry.DataBegin < before.DataEnd)
        {
            for (FileTableEntry* overlapping : {&before, &entry})
            {
                if (overlapping->LeftOut)
                    continue;
                Logger::Log::Warning("Zip archive entry '{}' shares its data with another entry and is skipped",
                                     overlapping->Name);
                overlapping->LeftOut = true;
            }
        }
        if (entry.DataEnd > before.DataEnd)
            furthest = at;
    }
}

// Leaves out every entry whose name an earlier entry has, so that a name finds one entry and no
// two entries write one file. Names compare without case and in one Unicode form, as Windows
// and macOS find files.
void LeaveOutRepeatedNames(std::vector<FileTableEntry>& entries)
{
    std::vector<std::string> folded;
    folded.reserve(entries.size());
    for (const FileTableEntry& entry : entries)
        folded.push_back(AssetPaths::CaseFoldUtf8(entry.Name));
    std::vector<size_t> byName(entries.size());
    std::iota(byName.begin(), byName.end(), size_t{0});
    std::stable_sort(byName.begin(), byName.end(),
                     [&folded](size_t left, size_t right) { return folded[left] < folded[right]; });
    for (size_t at = 1; at < byName.size(); ++at)
    {
        FileTableEntry& entry = entries[byName[at]];
        if (folded[byName[at]] != folded[byName[at - 1]] || entry.LeftOut)
            continue;
        Logger::Log::Warning("Zip archive entry '{}' repeats the name of an earlier entry and is skipped", entry.Name);
        entry.LeftOut = true;
    }
}

// Decompresses one entry into destination through a temporary file beside it, so that a failed
// entry leaves no partial file and keeps a file already at destination. Returns the reason on
// failure.
std::string ExtractEntry(mz_zip_archive* archive, mz_uint index, const fs::path& destination)
{
    std::error_code ec;
    fs::create_directories(destination.parent_path(), ec);
    fs::path partial = destination;
    partial += kPartialFileSuffix;
    errno = 0;
    std::ofstream out(partial, std::ios::binary | std::ios::trunc);
    if (!out)
        return WriteFailure(destination, ec ? ec : LastSystemError());

    const bool extracted = mz_zip_reader_extract_to_callback(archive, index, WriteToStream, &out, 0);
    std::error_code writeError;
    if (!out.good() || (!extracted && mz_zip_get_last_error(archive) == MZ_ZIP_WRITE_CALLBACK_FAILED))
        writeError = LastSystemError();
    out.close();
    if (!writeError && out.fail())
        writeError = LastSystemError();
    if (extracted && !writeError)
    {
        fs::rename(partial, destination, writeError);
        if (!writeError)
            return {};
    }

    fs::remove(partial, ec);
    if (writeError)
        return WriteFailure(destination, writeError);
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

    // The bytes from an entry's local header to the end of its data. The local header's name
    // and extra-field lengths are its own and can differ from the central directory's.
    std::optional<mz_uint64> LocalEntryLength(const mz_zip_archive_file_stat& stat)
    {
        std::array<unsigned char, kLocalHeaderSize> header{};
        if (Archive.m_pRead(Archive.m_pIO_opaque, stat.m_local_header_ofs, header.data(), header.size()) !=
            header.size())
            return std::nullopt;
        return kLocalHeaderSize + ReadUint16(header, kLocalNameLengthOffset) +
               ReadUint16(header, kLocalExtraLengthOffset) + stat.m_comp_size;
    }

    // The file entries that can be extracted, in archive order: names, miniz indices and
    // declared sizes. Names that are not UTF-8, unsafe names, encrypted entries, entries the
    // reader cannot decompress, a repeat of an earlier name and entries that share their bytes
    // are skipped with a warning; folder entries are skipped silently.
    bool ReadFileTable(std::string* outError)
    {
        const mz_uint fileCount = mz_zip_reader_get_num_files(&Archive);
        std::vector<FileTableEntry> entries;
        entries.reserve(fileCount);
        for (mz_uint index = 0; index < fileCount; ++index)
        {
            mz_zip_archive_file_stat stat{};
            std::optional<std::string> name;
            if (mz_zip_reader_file_stat(&Archive, index, &stat))
                name = ReadEntryName(&Archive, index);
            if (!name)
            {
                if (outError)
                    *outError = "Could not read the archive's file table.";
                return false;
            }
            if (stat.m_is_directory)
                continue;
            if (!IsValidUtf8(*name))
            {
                Logger::Log::Warning("Zip archive entry '{}' has a name that is not UTF-8 and is skipped; zip the "
                                     "files again with a tool that writes UTF-8 names",
                                     *name);
                continue;
            }
            if (!IsSafeEntryName(*name))
            {
                Logger::Log::Warning("Zip archive entry '{}' has an unsafe name and is skipped", *name);
                continue;
            }
            if (stat.m_is_encrypted)
            {
                Logger::Log::Warning("Zip archive entry '{}' is encrypted and is skipped", *name);
                continue;
            }
            if (!UsesSupportedCompression(stat))
            {
                Logger::Log::Warning(
                    "Zip archive entry '{}' uses a compression method the reader does not support and is skipped",
                    *name);
                continue;
            }
            const std::optional<mz_uint64> length = LocalEntryLength(stat);
            if (!length)
            {
                if (outError)
                    *outError = "Could not read the archive's file table.";
                return false;
            }
            entries.push_back({std::move(*name), index, stat.m_uncomp_size, stat.m_local_header_ofs,
                               stat.m_local_header_ofs + *length});
        }
        LeaveOutOverlappingEntries(entries);
        LeaveOutRepeatedNames(entries);

        Names.reserve(entries.size());
        Indices.reserve(entries.size());
        for (FileTableEntry& entry : entries)
        {
            if (entry.LeftOut)
                continue;
            Names.push_back(std::move(entry.Name));
            Indices.push_back(entry.Index);
            Sizes.push_back(entry.Size);
        }

        // A case-sensitive mz_zip_reader_locate_file is a linear scan, so one
        // lookup per requested entry would be quadratic; look names up here instead.
        ByName.resize(Names.size());
        std::iota(ByName.begin(), ByName.end(), size_t{0});
        std::sort(ByName.begin(), ByName.end(),
                  [this](size_t left, size_t right) { return Names[left] < Names[right]; });
        return true;
    }

    // The position in Names of the file named name, or nothing.
    std::optional<size_t> FindFile(const std::string& name) const
    {
        const auto found = std::lower_bound(
            ByName.begin(), ByName.end(), name,
            [this](size_t position, const std::string& key) { return Names[position] < key; });
        if (found == ByName.end() || Names[*found] != name)
            return std::nullopt;
        return *found;
    }

    FILE* File = nullptr;
    mz_zip_archive Archive{};
    bool ReaderOpen = false;
    std::vector<std::string> Names;
    // The miniz index of each name, parallel to Names.
    std::vector<mz_uint> Indices;
    // The size each name's entry declares, parallel to Names.
    std::vector<mz_uint64> Sizes;
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
            *outError = "Could not open archive: " + PathToUtf8(zipFile);
        return std::nullopt;
    }
    state->ReaderOpen = mz_zip_reader_init_cfile(&state->Archive, state->File, 0, kReaderFlags);
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
    state->ReaderOpen = mz_zip_reader_init_mem(&state->Archive, archive.data(), archive.size(), kReaderFlags);
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
                                      std::vector<ZipEntryFailure>& outFailures, std::uint64_t maxTotalBytes)
{
    // The position in Names of each requested entry, or nothing for a name the archive cannot
    // extract, whether each name is safe, and the bytes the found entries declare together.
    std::vector<std::optional<size_t>> positions;
    positions.reserve(entries.size());
    std::vector<bool> safeNames;
    safeNames.reserve(entries.size());
    std::uint64_t totalBytes = 0;
    for (const ZipEntryExtraction& entry : entries)
    {
        safeNames.push_back(IsSafeEntryName(entry.EntryName));
        positions.push_back(safeNames.back() ? m_State->FindFile(entry.EntryName) : std::nullopt);
        if (!positions.back())
            continue;
        const std::uint64_t size = m_State->Sizes[*positions.back()];
        totalBytes = size > std::numeric_limits<std::uint64_t>::max() - totalBytes
                         ? std::numeric_limits<std::uint64_t>::max()
                         : totalBytes + size;
    }
    const bool pastLimit = totalBytes > maxTotalBytes;

    const size_t failureCountBefore = outFailures.size();
    for (size_t at = 0; at < entries.size(); ++at)
    {
        const ZipEntryExtraction& entry = entries[at];
        if (!positions[at])
        {
            outFailures.push_back({entry.EntryName, safeNames[at] ? "The archive has no file with this name."
                                                                  : "The entry name is not a safe relative path."});
            continue;
        }
        if (pastLimit)
        {
            outFailures.push_back({entry.EntryName, LimitFailure(totalBytes, maxTotalBytes), true});
            continue;
        }
        std::string message = ExtractEntry(&m_State->Archive, m_State->Indices[*positions[at]], entry.Destination);
        if (!message.empty())
            outFailures.push_back({entry.EntryName, std::move(message)});
    }
    return outFailures.size() == failureCountBefore;
}

} // namespace GameEngine::Editor
