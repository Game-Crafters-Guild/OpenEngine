#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace GameEngine::Editor
{

/// One file to extract from a .zip archive.
struct ZipEntryExtraction
{
    /// The entry's name in the archive, '/'-separated, as ListZipEntries returns it.
    std::string EntryName;
    /// The file to write. Missing parent folders are created; an existing file is replaced.
    std::filesystem::path Destination;
};

/// An entry ExtractZipEntries did not extract, and why.
struct ZipEntryFailure
{
    std::string EntryName;
    std::string Message;
};

/// Lists the names of the archive's files in archive order ('/'-separated, relative).
/// Folder entries are left out. Unsafe names (absolute, with a ".." or "." segment, or with
/// '\' or ':') are left out with a warning, so any returned name can be appended to a
/// destination folder without escaping it. Returns false with outError when the archive
/// cannot be read.
bool ListZipEntries(const std::filesystem::path& zipFile,
                    std::vector<std::string>& outEntryNames,
                    std::string* outError);

/// Extracts each listed entry to its destination. An entry that is not a file in the archive,
/// has an unsafe name, or cannot be decompressed or written is added to outFailures, and the
/// other entries are still extracted. Returns true when every entry was extracted. Returns false
/// with outError (and no failures) when the archive cannot be read.
bool ExtractZipEntries(const std::filesystem::path& zipFile,
                       std::span<const ZipEntryExtraction> entries,
                       std::vector<ZipEntryFailure>& outFailures,
                       std::string* outError);

/// Extracts a .zip archive into destination. When innerRoot is non-empty, only
/// entries under that archive-relative folder are extracted, with the prefix
/// stripped (a manifest zip source's "path"). Unsafe entry names are skipped as in
/// ListZipEntries, so a hostile archive can never write outside destination.
/// Returns false with outError when the archive cannot be read, has nothing to
/// extract, or an entry fails; the message names the first entry that failed.
bool ExtractZipArchive(const std::filesystem::path& zipFile,
                       const std::filesystem::path& destination,
                       const std::string& innerRoot,
                       std::string* outError);

} // namespace GameEngine::Editor
