#pragma once

#include <cstddef>
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
    /// The file to write. Missing parent folders are created; an existing file is replaced.
    std::filesystem::path Destination;
};

/// An entry ZipArchiveReader::ExtractEntries did not extract, and why.
struct ZipEntryFailure
{
    std::string EntryName;
    std::string Message;
};

/// An open .zip archive, read from a file or from memory. Its files can be extracted to files
/// or read into memory.
///
/// Entry names are checked when the archive opens. Unsafe names (absolute, with a ".." or
/// "." segment, or with '\' or ':') are left out with a warning, so any name the reader gives
/// can be appended to a destination folder without escaping it, and no request can extract or
/// read one.
///
/// A reader is used from one thread at a time.
class ZipArchiveReader
{
  public:
    /// Opens the archive at zipFile. Returns no reader, with outError, when the file cannot be
    /// opened or is not a .zip archive.
    static std::optional<ZipArchiveReader> OpenFile(const std::filesystem::path& zipFile,
                                                    std::string* outError);

    /// Opens an archive held in memory, without copying it: the bytes must stay alive and
    /// unchanged while the reader exists. Returns no reader, with outError, when the bytes are
    /// not a .zip archive.
    static std::optional<ZipArchiveReader> OpenMemory(std::span<const std::byte> archive,
                                                      std::string* outError);

    ZipArchiveReader(ZipArchiveReader&& other) noexcept;
    ZipArchiveReader& operator=(ZipArchiveReader&& other) noexcept;
    ~ZipArchiveReader();

    /// The names of the archive's files in archive order ('/'-separated, relative). Folder
    /// entries and unsafe names are left out.
    const std::vector<std::string>& EntryNames() const;

    /// Extracts each listed entry to its destination. An entry that is not a file in the
    /// archive, has an unsafe name, or cannot be decompressed or written is added to
    /// outFailures, and the other entries are still extracted. Returns true when every entry
    /// was extracted.
    bool ExtractEntries(std::span<const ZipEntryExtraction> entries,
                        std::vector<ZipEntryFailure>& outFailures);

    /// Reads one file entry into outBytes, replacing its contents. maxBytes caps the size: an
    /// archive states each entry's size itself, so a hostile archive could otherwise ask for any
    /// allocation. Returns false with outError, and outBytes empty, when the entry is not a file
    /// in the archive, has an unsafe name, is larger than maxBytes, or cannot be decompressed.
    bool ReadEntry(const std::string& entryName, std::vector<std::byte>& outBytes, size_t maxBytes,
                   std::string* outError);

  private:
    struct State;

    explicit ZipArchiveReader(std::unique_ptr<State> state);

    std::unique_ptr<State> m_State;
};

} // namespace GameEngine::Editor
