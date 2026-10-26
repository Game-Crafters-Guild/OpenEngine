#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace GameEngine::Editor
{

/// One file to extract from a .zip archive.
struct ZipEntryExtraction
{
    /// The entry's name in the archive, '/'-separated, as ZipArchiveReader::EntryNames gives it.
    std::string EntryName;
    /// The file to write. Missing parent folders are created. The entry is written to the same path
    /// with ".partial" appended and renamed to this path when it is whole, so an existing file is
    /// replaced only by a whole entry.
    std::filesystem::path Destination;
};

/// An entry ZipArchiveReader::ExtractEntries did not extract, and why.
struct ZipEntryFailure
{
    /// The entry's name as the request gave it.
    std::string EntryName;
    /// Why the entry was not extracted, for a person to read.
    std::string Message;
    /// True when no entry was written because the chosen entries together declare more than the
    /// call's limit. Fewer entries or a larger limit extract them.
    bool PastSizeLimit = false;
};

/// An open .zip archive, read from a file or from memory.
///
/// Entry names are checked when the archive opens. Unsafe names are left out with a warning:
/// names that are not UTF-8 (an older zip tool can write names in a code page such as 437),
/// absolute names, names with '\', ':' or a null character, and names with a
/// segment that ends in '.' or a space (so "." and "..") or that Windows opens as a device
/// (CON, PRN, AUX, NUL, CONIN$, CONOUT$, COM0 to COM9, LPT0 to LPT9, and COM and LPT with a
/// superscript 1, 2 or 3, in any case and with any extension).
/// Any name the reader gives can be appended to a destination folder, through PathFromUtf8,
/// and the file stays in that folder on every platform; no request can extract an unsafe name.
/// Also left out with a warning: an encrypted entry, an entry whose compression the reader
/// does not support, an entry whose name repeats an earlier entry's without regard to case or
/// Unicode form, and entries whose bytes overlap another entry's (a zip bomb's construction).
///
/// A reader is used from one thread at a time.
class ZipArchiveReader
{
  public:
    /// The most one ExtractEntries call writes by default, counted by the entries' declared
    /// sizes. A hostile archive is small: deflate lets one entry expand about 1032 times, so a
    /// 1 MB download can write 1 GB. A caller that extracts a larger pack passes a larger limit.
    static constexpr std::uint64_t kDefaultMaxExtractedBytes = std::uint64_t{512} << 20;

    /// Opens the archive at zipFile. Returns no reader, with outError, when the file cannot be
    /// opened or is not a .zip archive.
    static std::optional<ZipArchiveReader> OpenFile(const std::filesystem::path& zipFile,
                                                    std::string* outError);

    /// Opens an archive held in memory, without copying it: the bytes must stay alive and
    /// unchanged while the reader exists. ExtractEntries reads them; the entry names are copied
    /// when the archive opens. Returns no reader, with outError, when the bytes are not a .zip
    /// archive.
    static std::optional<ZipArchiveReader> OpenMemory(std::span<const std::byte> archive,
                                                      std::string* outError);

    ZipArchiveReader(ZipArchiveReader&& other) noexcept;
    ZipArchiveReader& operator=(ZipArchiveReader&& other) noexcept;
    ~ZipArchiveReader();

    /// The names of the archive's files in archive order ('/'-separated, relative). Folder
    /// entries and unsafe names are left out.
    const std::vector<std::string>& EntryNames() const;

    /// Extracts each listed entry to its destination. When the found entries together declare
    /// more than maxTotalBytes, no entry is written and each is added to outFailures with
    /// PastSizeLimit set. Otherwise an entry that is not a file in the archive, has an unsafe
    /// name, or cannot be decompressed or written is added to outFailures and leaves no file of
    /// its own, and the other entries are still extracted. Returns true when every entry was
    /// extracted.
    bool ExtractEntries(std::span<const ZipEntryExtraction> entries,
                        std::vector<ZipEntryFailure>& outFailures,
                        std::uint64_t maxTotalBytes = kDefaultMaxExtractedBytes);

  private:
    struct State;

    explicit ZipArchiveReader(std::unique_ptr<State> state);

    std::unique_ptr<State> m_State;
};

} // namespace GameEngine::Editor
