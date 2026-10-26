#pragma once

#include <cstddef>
#include <cstdint>
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
    // miniz reads the path as UTF-8 on every platform.
    const std::u8string zipPath = zipFile.u8string();
    mz_zip_archive archive{};
    if (!mz_zip_writer_init_file(&archive, reinterpret_cast<const char*>(zipPath.c_str()), 0))
        return false;
    bool written = true;
    for (const auto& [name, contents] : entries)
    {
        written = written && mz_zip_writer_add_mem(&archive, name.c_str(), contents.data(),
                                                   contents.size(), MZ_DEFAULT_LEVEL);
    }
    written = written && mz_zip_writer_finalize_archive(&archive);
    return mz_zip_writer_end(&archive) && written;
}

// The width-byte little-endian number at bytes[at].
inline uint32_t ReadLittleEndian(const std::string& bytes, size_t at, size_t width)
{
    uint32_t value = 0;
    for (size_t byte = 0; byte < width; ++byte)
        value |= static_cast<uint32_t>(static_cast<unsigned char>(bytes[at + byte])) << (8 * byte);
    return value;
}

inline void WriteLittleEndian(std::string& bytes, size_t at, size_t width, uint32_t value)
{
    for (size_t byte = 0; byte < width; ++byte)
        bytes[at + byte] = static_cast<char>((value >> (8 * byte)) & 0xFF);
}

// Sets the size each entry of archiveBytes, an archive WriteTestZip wrote, declares in its local
// header and in the central directory to size, and leaves the data as it is. The offsets are
// the fixed header fields of PKWARE APPNOTE.TXT 4.3.7, 4.3.12 and 4.3.16; the archive has no
// comment and no zip64 records.
inline void SetDeclaredSizes(std::string& archiveBytes, uint32_t size)
{
    constexpr size_t kEndRecordSize = 22;
    constexpr size_t kEndEntryCountOffset = 10;
    constexpr size_t kEndCentralDirectoryOffset = 16;
    constexpr size_t kCentralHeaderSize = 46;
    constexpr size_t kCentralSizeOffset = 24;
    constexpr size_t kCentralNameLengthOffset = 28;
    constexpr size_t kCentralExtraLengthOffset = 30;
    constexpr size_t kCentralCommentLengthOffset = 32;
    constexpr size_t kCentralLocalHeaderOffset = 42;
    constexpr size_t kLocalSizeOffset = 22;

    const size_t endRecord = archiveBytes.size() - kEndRecordSize;
    const uint32_t entryCount = ReadLittleEndian(archiveBytes, endRecord + kEndEntryCountOffset, 2);
    size_t central = ReadLittleEndian(archiveBytes, endRecord + kEndCentralDirectoryOffset, 4);
    for (uint32_t entry = 0; entry < entryCount; ++entry)
    {
        WriteLittleEndian(archiveBytes, central + kCentralSizeOffset, 4, size);
        const size_t local = ReadLittleEndian(archiveBytes, central + kCentralLocalHeaderOffset, 4);
        WriteLittleEndian(archiveBytes, local + kLocalSizeOffset, 4, size);
        central += kCentralHeaderSize + ReadLittleEndian(archiveBytes, central + kCentralNameLengthOffset, 2) +
                   ReadLittleEndian(archiveBytes, central + kCentralExtraLengthOffset, 2) +
                   ReadLittleEndian(archiveBytes, central + kCentralCommentLengthOffset, 2);
    }
}
