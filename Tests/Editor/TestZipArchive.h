#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <miniz.h>

// An entry of a test archive: its name and contents. A name ending in '/' is a folder entry.
using TestZipEntry = std::pair<std::string, std::string>;

// Writes the entries to zipFile as a .zip archive, in order, with the vendored miniz writer.
// Returns false when miniz refuses an entry (a name that starts with '/') or cannot write.
inline bool WriteTestZip(const std::filesystem::path& zipFile, const std::vector<TestZipEntry>& entries)
{
    mz_zip_archive archive{};
    if (!mz_zip_writer_init_file(&archive, zipFile.string().c_str(), 0))
        return false;
    bool written = true;
    for (const auto& [name, contents] : entries)
    {
        written = written && mz_zip_writer_add_mem(&archive, name.c_str(), contents.data(),
                                                   contents.size(), MZ_DEFAULT_COMPRESSION);
    }
    written = written && mz_zip_writer_finalize_archive(&archive);
    return mz_zip_writer_end(&archive) && written;
}
