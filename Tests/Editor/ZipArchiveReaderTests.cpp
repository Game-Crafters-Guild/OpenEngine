// The editor's zip reader as package modules see it through EditorSDK: opening an
// archive on disk or in memory, listing its files, extracting a chosen set of them,
// and refusing entry names that would write outside the destination.

#include <gtest/gtest.h>

#include "Editor/ZipArchiveReader.h"
#include "TestZipArchive.h"
#include "Types/PathUtils.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using GameEngine::PathFromUtf8;
using GameEngine::Editor::ZipArchiveReader;
using GameEngine::Editor::ZipEntryExtraction;
using GameEngine::Editor::ZipEntryFailure;

namespace
{

// Byte offsets of the zip format's fixed header fields (PKWARE APPNOTE.TXT 4.3.7, 4.3.12, 4.3.16).
// The archives the tests write have no archive comment and no zip64 records.
constexpr size_t kLocalFlagsOffset = 6;
constexpr size_t kLocalMethodOffset = 8;
constexpr size_t kLocalUncompressedSizeOffset = 22;
constexpr size_t kCentralHeaderSize = 46;
constexpr size_t kCentralFlagsOffset = 8;
constexpr size_t kCentralMethodOffset = 10;
constexpr size_t kCentralCrcOffset = 16;
constexpr size_t kCentralUncompressedSizeOffset = 24;
constexpr size_t kCentralNameLengthOffset = 28;
constexpr size_t kCentralExtraLengthOffset = 30;
constexpr size_t kCentralCommentLengthOffset = 32;
constexpr size_t kCentralLocalHeaderOffset = 42;
constexpr size_t kEndRecordSize = 22;
constexpr size_t kEndEntriesOnDiskOffset = 8;
constexpr size_t kEndEntryCountOffset = 10;
constexpr size_t kEndCentralDirectoryOffset = 16;
constexpr uint32_t kEncryptedFlag = 1;
constexpr uint32_t kLocalHeaderSignature = 0x04034B50;
constexpr uint32_t kCentralHeaderSignature = 0x02014B50;
constexpr uint32_t kEndRecordSignature = 0x06054B50;
constexpr uint32_t kZipVersion = 20;
constexpr uint32_t kDeflateMethod = 8;
constexpr uint32_t kLzmaMethod = 14;

// The position of the entryIndex-th central directory header.
size_t CentralHeaderAt(const std::string& bytes, size_t entryIndex)
{
    const size_t endRecord = bytes.size() - kEndRecordSize;
    size_t at = ReadLittleEndian(bytes, endRecord + kEndCentralDirectoryOffset, 4);
    for (size_t index = 0; index < entryIndex; ++index)
    {
        at += kCentralHeaderSize + ReadLittleEndian(bytes, at + kCentralNameLengthOffset, 2) +
              ReadLittleEndian(bytes, at + kCentralExtraLengthOffset, 2) +
              ReadLittleEndian(bytes, at + kCentralCommentLengthOffset, 2);
    }
    return at;
}

size_t CountFiles(const fs::path& folder)
{
    return static_cast<size_t>(std::distance(fs::directory_iterator(folder), fs::directory_iterator()));
}

void AppendLittleEndian(std::string& bytes, size_t width, uint32_t value)
{
    for (size_t byte = 0; byte < width; ++byte)
        bytes.push_back(static_cast<char>((value >> (8 * byte)) & 0xFF));
}

// The fields a local header and a central directory header share, from the version needed to
// the extra-field length.
void AppendSharedHeaderFields(std::string& bytes, const std::string& name, uint32_t crc, uint32_t compressedSize,
                              uint32_t size, uint32_t extraLength)
{
    AppendLittleEndian(bytes, 2, kZipVersion);
    AppendLittleEndian(bytes, 2, 0);
    AppendLittleEndian(bytes, 2, kDeflateMethod);
    AppendLittleEndian(bytes, 4, 0);
    AppendLittleEndian(bytes, 4, crc);
    AppendLittleEndian(bytes, 4, compressedSize);
    AppendLittleEndian(bytes, 4, size);
    AppendLittleEndian(bytes, 2, static_cast<uint32_t>(name.size()));
    AppendLittleEndian(bytes, 2, extraLength);
}

// An archive of a.bin and b.bin, each a megabyte of zeros, that stores the deflated data once:
// b.bin's local header sits inside a.bin's local extra field, and both headers lead to the
// same data. The two entries' local headers, counted without their names and extra fields, do
// not overlap.
std::string ArchiveWithAnEntryHiddenInAnExtraField()
{
    const std::string zeros(size_t{1} << 20, '\0');
    size_t compressedSize = 0;
    void* compressed = tdefl_compress_mem_to_heap(zeros.data(), zeros.size(), &compressedSize, TDEFL_DEFAULT_MAX_PROBES);
    const std::string data(static_cast<const char*>(compressed), compressedSize);
    mz_free(compressed);
    const auto crc = static_cast<uint32_t>(
        mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(zeros.data()), zeros.size()));
    const auto dataSize = static_cast<uint32_t>(data.size());
    const auto size = static_cast<uint32_t>(zeros.size());

    std::string hidden;
    AppendLittleEndian(hidden, 4, kLocalHeaderSignature);
    AppendSharedHeaderFields(hidden, "b.bin", crc, dataSize, size, 0);
    hidden += "b.bin";
    // a.bin's extra field ends where b.bin's local header ends, so both lead to the data.
    const std::string filler(dataSize - 5, 'E');

    std::string bytes;
    AppendLittleEndian(bytes, 4, kLocalHeaderSignature);
    AppendSharedHeaderFields(bytes, "a.bin", crc, dataSize, size, static_cast<uint32_t>(filler.size() + hidden.size()));
    bytes += "a.bin" + filler;
    const auto hiddenOffset = static_cast<uint32_t>(bytes.size());
    bytes += hidden + data;

    std::string central;
    for (const auto& [name, offset] : {std::pair<std::string, uint32_t>{"a.bin", 0}, {"b.bin", hiddenOffset}})
    {
        AppendLittleEndian(central, 4, kCentralHeaderSignature);
        AppendLittleEndian(central, 2, kZipVersion);
        AppendSharedHeaderFields(central, name, crc, dataSize, size, 0);
        AppendLittleEndian(central, 2, 0);
        AppendLittleEndian(central, 2, 0);
        AppendLittleEndian(central, 2, 0);
        AppendLittleEndian(central, 4, 0);
        AppendLittleEndian(central, 4, offset);
        central += name;
    }
    const auto centralOffset = static_cast<uint32_t>(bytes.size());
    bytes += central;
    AppendLittleEndian(bytes, 4, kEndRecordSignature);
    AppendLittleEndian(bytes, 4, 0);
    AppendLittleEndian(bytes, 2, 2);
    AppendLittleEndian(bytes, 2, 2);
    AppendLittleEndian(bytes, 4, static_cast<uint32_t>(central.size()));
    AppendLittleEndian(bytes, 4, centralOffset);
    AppendLittleEndian(bytes, 2, 0);
    return bytes;
}

class ZipArchiveReaderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        m_Root = fs::temp_directory_path() / ("ge_zip_archive_" + std::to_string(stamp));
        fs::create_directories(m_Root);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(m_Root, ec);
    }

    fs::path WriteArchive(const std::vector<TestZipEntry>& entries)
    {
        const fs::path zipFile = m_Root / "archive.zip";
        EXPECT_TRUE(WriteTestZip(zipFile, entries));
        return zipFile;
    }

    // miniz refuses to write a name that starts with '/' and stops a name at a null character,
    // so such a name is written as placeholderName and patched to patchedName (the same length)
    // in the local header and the central directory.
    fs::path WriteArchiveWithPatchedName(const std::string& placeholderName, const std::string& patchedName,
                                         const std::vector<TestZipEntry>& otherEntries)
    {
        std::vector<TestZipEntry> entries = otherEntries;
        entries.emplace_back(placeholderName, "patched");
        const fs::path zipFile = WriteArchive(entries);

        std::string bytes = ReadFile(zipFile);
        size_t patched = 0;
        for (size_t at = bytes.find(placeholderName); at != std::string::npos;
             at = bytes.find(placeholderName, at + 1))
        {
            bytes.replace(at, patchedName.size(), patchedName);
            ++patched;
        }
        EXPECT_EQ(patched, 2u);
        WriteFile(zipFile, bytes);
        return zipFile;
    }

    static void WriteFile(const fs::path& file, const std::string& bytes)
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    static std::string ReadFile(const fs::path& file)
    {
        std::ifstream in(file, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    fs::path m_Root;
};

} // namespace

TEST_F(ZipArchiveReaderTest, ListsFilesInArchiveOrderWithoutFolders)
{
    const fs::path zipFile = WriteArchive({{"readme.txt", "read me"},
                                           {"Models/", ""},
                                           {"Models/GLB format/tree.glb", "glb"},
                                           {"Audio/click.ogg", "ogg"}});

    std::string error;
    const std::optional<ZipArchiveReader> archive = ZipArchiveReader::OpenFile(zipFile, &error);
    ASSERT_TRUE(archive) << error;
    EXPECT_EQ(archive->EntryNames(), (std::vector<std::string>{
                                         "readme.txt", "Models/GLB format/tree.glb", "Audio/click.ogg"}));
}

TEST_F(ZipArchiveReaderTest, LeavesOutUnsafeNames)
{
    const fs::path zipFile = WriteArchiveWithPatchedName("_absolute.txt", "/absolute.txt",
                                                         {{"../escape.txt", "x"},
                                                          {"Models/../../escape.txt", "x"},
                                                          {"./here.txt", "x"},
                                                          {"Models\\windows.txt", "x"},
                                                          {"C:/drive.txt", "x"},
                                                          {"Models/safe.txt", "safe"}});

    std::string error;
    const std::optional<ZipArchiveReader> archive = ZipArchiveReader::OpenFile(zipFile, &error);
    ASSERT_TRUE(archive) << error;
    EXPECT_EQ(archive->EntryNames(), (std::vector<std::string>{"Models/safe.txt"}));
}

// miniz stops a name at a null character; the name past it is part of the entry's name. The
// null is the name's only fault.
TEST_F(ZipArchiveReaderTest, LeavesOutANameWithANullCharacter)
{
    const fs::path zipFile =
        WriteArchiveWithPatchedName("safe.txt_.glb", std::string("safe.txt\0.glb", 13), {});
    std::string error;
    const std::optional<ZipArchiveReader> archive = ZipArchiveReader::OpenFile(zipFile, &error);
    ASSERT_TRUE(archive) << error;
    EXPECT_TRUE(archive->EntryNames().empty());
}

// An older zip tool writes a name in a code page such as 437 ("caf\x82.txt" for café.txt), and
// no path can be made from it, so the name is left out like an unsafe one.
TEST_F(ZipArchiveReaderTest, LeavesOutANameThatIsNotUtf8)
{
    const fs::path zipFile =
        WriteArchiveWithPatchedName("caf_.txt", "caf\x82.txt", {{"caf\xC3\xA9.txt", "cafe"}});
    std::string error;
    std::optional<ZipArchiveReader> archive = ZipArchiveReader::OpenFile(zipFile, &error);
    ASSERT_TRUE(archive) << error;
    EXPECT_EQ(archive->EntryNames(), (std::vector<std::string>{"caf\xC3\xA9.txt"}));
    for (const std::string& name : archive->EntryNames())
        EXPECT_NO_THROW((void)PathFromUtf8(name)) << testing::PrintToString(name);

    std::vector<ZipEntryFailure> failures;
    EXPECT_FALSE(archive->ExtractEntries(
        std::vector<ZipEntryExtraction>{{"caf\x82.txt", m_Root / "out" / "cafe.txt"}}, failures));
    ASSERT_EQ(failures.size(), 1u);
    EXPECT_NE(failures[0].Message.find("not a safe relative path"), std::string::npos) << failures[0].Message;
    EXPECT_FALSE(fs::exists(m_Root / "out"));
}

TEST_F(ZipArchiveReaderTest, OpenReportsBytesThatAreNotAnArchive)
{
    const fs::path notZip = m_Root / "download.zip";
    std::ofstream(notZip) << "not a zip";

    std::string error;
    EXPECT_FALSE(ZipArchiveReader::OpenFile(notZip, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(ZipArchiveReader::OpenFile(m_Root / "missing.zip", &error));
    EXPECT_NE(error.find("missing.zip"), std::string::npos) << error;

    const std::string bytes = "not a zip";
    error.clear();
    EXPECT_FALSE(ZipArchiveReader::OpenMemory(std::as_bytes(std::span(bytes)), &error));
    EXPECT_FALSE(error.empty());
}

TEST_F(ZipArchiveReaderTest, ExtractsOnlyTheChosenEntriesToTheirDestinations)
{
    const fs::path zipFile = WriteArchive({{"Models/GLB format/tree.glb", "glb"},
                                           {"Models/FBX format/tree.fbx", "fbx"},
                                           {"License.txt", "license"}});
    const fs::path output = m_Root / "Project" / "Assets" / "Kenney";
    fs::create_directories(output);
    std::ofstream(output / "License.txt") << "an older license that is replaced";

    std::string error;
    std::optional<ZipArchiveReader> archive = ZipArchiveReader::OpenFile(zipFile, &error);
    ASSERT_TRUE(archive) << error;
    const std::vector<ZipEntryExtraction> chosen = {
        {"Models/GLB format/tree.glb", output / "Models" / "tree.glb"},
        {"License.txt", output / "License.txt"}};
    std::vector<ZipEntryFailure> failures;
    EXPECT_TRUE(archive->ExtractEntries(chosen, failures));
    EXPECT_TRUE(failures.empty());

    EXPECT_EQ(ReadFile(output / "Models" / "tree.glb"), "glb");
    EXPECT_EQ(ReadFile(output / "License.txt"), "license");
    EXPECT_FALSE(fs::exists(output / "Models" / "FBX format"));
    EXPECT_FALSE(fs::exists(output / "Models" / "tree.fbx"));
}

TEST_F(ZipArchiveReaderTest, ExtractReportsEachFailedEntryAndExtractsTheRest)
{
    const fs::path zipFile = WriteArchive({{"Models/", ""},
                                           {"Models/tree.glb", "glb"},
                                           {"../escape.txt", "escaped"},
                                           {"Audio/click.ogg", "ogg"}});
    const fs::path output = m_Root / "out";
    fs::create_directories(output / "click.ogg");

    std::string error;
    std::optional<ZipArchiveReader> archive = ZipArchiveReader::OpenFile(zipFile, &error);
    ASSERT_TRUE(archive) << error;
    const std::vector<ZipEntryExtraction> chosen = {
        {"Models/missing.glb", output / "missing.glb"},
        {"Models/", output / "Models"},
        {"../escape.txt", output / "escape.txt"},
        {"Audio/click.ogg", output / "click.ogg"},
        {"Models/tree.glb", output / "tree.glb"}};
    std::vector<ZipEntryFailure> failures;
    EXPECT_FALSE(archive->ExtractEntries(chosen, failures));

    ASSERT_EQ(failures.size(), 4u);
    EXPECT_EQ(failures[0].EntryName, "Models/missing.glb");
    EXPECT_EQ(failures[1].EntryName, "Models/");
    EXPECT_EQ(failures[2].EntryName, "../escape.txt");
    EXPECT_EQ(failures[3].EntryName, "Audio/click.ogg");
    for (const ZipEntryFailure& failure : failures)
    {
        EXPECT_FALSE(failure.Message.empty()) << failure.EntryName;
        EXPECT_FALSE(failure.PastSizeLimit) << failure.EntryName;
    }
    EXPECT_NE(failures[3].Message.find("click.ogg"), std::string::npos) << failures[3].Message;

    EXPECT_EQ(ReadFile(output / "tree.glb"), "glb");
    EXPECT_FALSE(fs::exists(output / "escape.txt"));
    EXPECT_FALSE(fs::exists(m_Root / "escape.txt"));
}

TEST_F(ZipArchiveReaderTest, ReadsAnArchiveHeldInMemory)
{
    const std::string bytes = ReadFile(WriteArchive({{"Models/", ""},
                                                     {"Models/tree.glb", "glb"},
                                                     {"../escape.txt", "escaped"}}));

    std::string error;
    std::optional<ZipArchiveReader> archive =
        ZipArchiveReader::OpenMemory(std::as_bytes(std::span(bytes)), &error);
    ASSERT_TRUE(archive) << error;
    EXPECT_EQ(archive->EntryNames(), (std::vector<std::string>{"Models/tree.glb"}));

    const std::vector<ZipEntryExtraction> chosen = {{"Models/tree.glb", m_Root / "out" / "tree.glb"}};
    std::vector<ZipEntryFailure> failures;
    EXPECT_TRUE(archive->ExtractEntries(chosen, failures));
    EXPECT_EQ(ReadFile(m_Root / "out" / "tree.glb"), "glb");
}

TEST_F(ZipArchiveReaderTest, OpenRefusesATruncatedArchive)
{
    const std::string bytes = ReadFile(WriteArchive({{"Models/tree.glb", "glb"}, {"License.txt", "license"}}));
    for (const size_t length : {bytes.size() - 1, bytes.size() / 2, size_t{4}})
    {
        const std::string truncated = bytes.substr(0, length);
        WriteFile(m_Root / "truncated.zip", truncated);
        std::string error;
        EXPECT_FALSE(ZipArchiveReader::OpenFile(m_Root / "truncated.zip", &error)) << length;
        EXPECT_FALSE(error.empty()) << length;
        error.clear();
        EXPECT_FALSE(ZipArchiveReader::OpenMemory(std::as_bytes(std::span(truncated)), &error)) << length;
        EXPECT_FALSE(error.empty()) << length;
    }
}

TEST_F(ZipArchiveReaderTest, OpenRefusesMoreEntriesThanTheDirectoryHolds)
{
    std::string bytes = ReadFile(WriteArchive({{"Models/tree.glb", "glb"}}));
    const size_t endRecord = bytes.size() - kEndRecordSize;
    WriteLittleEndian(bytes, endRecord + kEndEntriesOnDiskOffset, 2, 2);
    WriteLittleEndian(bytes, endRecord + kEndEntryCountOffset, 2, 2);

    std::string error;
    EXPECT_FALSE(ZipArchiveReader::OpenMemory(std::as_bytes(std::span(bytes)), &error));
    EXPECT_FALSE(error.empty());
}

TEST_F(ZipArchiveReaderTest, OpenRefusesAnEntryWhoseDataLiesPastTheEnd)
{
    std::string bytes = ReadFile(WriteArchive({{"Models/tree.glb", "glb"}, {"License.txt", "license"}}));
    WriteLittleEndian(bytes, CentralHeaderAt(bytes, 1) + kCentralLocalHeaderOffset, 4,
                      static_cast<uint32_t>(bytes.size()));

    std::string error;
    EXPECT_FALSE(ZipArchiveReader::OpenMemory(std::as_bytes(std::span(bytes)), &error));
    EXPECT_FALSE(error.empty());
}

// The limit is raised past the declared size, so the entry fails when its data ends short.
TEST_F(ZipArchiveReaderTest, AnEntryClaimingAHugeSizeFailsAndLeavesNoFile)
{
    std::string bytes = ReadFile(WriteArchive({{"huge.bin", std::string(4096, 'x')}, {"safe.txt", "safe"}}));
    const size_t central = CentralHeaderAt(bytes, 0);
    const size_t local = ReadLittleEndian(bytes, central + kCentralLocalHeaderOffset, 4);
    WriteLittleEndian(bytes, central + kCentralUncompressedSizeOffset, 4, 0xFFFFFFF0u);
    WriteLittleEndian(bytes, local + kLocalUncompressedSizeOffset, 4, 0xFFFFFFF0u);

    std::string error;
    std::optional<ZipArchiveReader> archive = ZipArchiveReader::OpenMemory(std::as_bytes(std::span(bytes)), &error);
    ASSERT_TRUE(archive) << error;
    const std::vector<ZipEntryExtraction> chosen = {{"huge.bin", m_Root / "out" / "huge.bin"},
                                                    {"safe.txt", m_Root / "out" / "safe.txt"}};
    std::vector<ZipEntryFailure> failures;
    EXPECT_FALSE(archive->ExtractEntries(chosen, failures, std::uint64_t{1} << 32));
    ASSERT_EQ(failures.size(), 1u);
    EXPECT_EQ(failures[0].EntryName, "huge.bin");
    EXPECT_FALSE(fs::exists(m_Root / "out" / "huge.bin"));
    EXPECT_EQ(ReadFile(m_Root / "out" / "safe.txt"), "safe");
}

// One entry writes at most its declared size, which deflate bounds at about 1032 times its
// compressed bytes; an archive multiplies that only by giving several entries the same data.
TEST_F(ZipArchiveReaderTest, EntriesThatShareTheirDataAreLeftOut)
{
    const std::string zeros(size_t{1} << 20, '\0');
    std::string bytes = ReadFile(WriteArchive({{"a.bin", zeros}, {"b.bin", zeros}, {"c.bin", zeros}}));
    WriteLittleEndian(bytes, CentralHeaderAt(bytes, 1) + kCentralLocalHeaderOffset, 4,
                      ReadLittleEndian(bytes, CentralHeaderAt(bytes, 0) + kCentralLocalHeaderOffset, 4));

    std::string error;
    std::optional<ZipArchiveReader> archive = ZipArchiveReader::OpenMemory(std::as_bytes(std::span(bytes)), &error);
    ASSERT_TRUE(archive) << error;
    EXPECT_EQ(archive->EntryNames(), (std::vector<std::string>{"c.bin"}));

    const std::vector<ZipEntryExtraction> chosen = {{"a.bin", m_Root / "a.bin"}, {"c.bin", m_Root / "c.bin"}};
    std::vector<ZipEntryFailure> failures;
    EXPECT_FALSE(archive->ExtractEntries(chosen, failures));
    ASSERT_EQ(failures.size(), 1u);
    EXPECT_EQ(failures[0].EntryName, "a.bin");
    EXPECT_FALSE(fs::exists(m_Root / "a.bin"));
    EXPECT_EQ(fs::file_size(m_Root / "c.bin"), zeros.size());
}

TEST_F(ZipArchiveReaderTest, AnEntryNameIsReadWholeUpToTheLongestTheFormatHolds)
{
    const std::string folder = "Long/" + std::string(64900, 'a');
    const fs::path zipFile = WriteArchive({{folder + "/one.txt", "one"}, {folder + "/two.txt", "two"}});

    std::string error;
    std::optional<ZipArchiveReader> archive = ZipArchiveReader::OpenFile(zipFile, &error);
    ASSERT_TRUE(archive) << error;
    EXPECT_EQ(archive->EntryNames(), (std::vector<std::string>{folder + "/one.txt", folder + "/two.txt"}));

    const std::vector<ZipEntryExtraction> chosen = {{folder + "/two.txt", m_Root / "two.txt"}};
    std::vector<ZipEntryFailure> failures;
    EXPECT_TRUE(archive->ExtractEntries(chosen, failures));
    EXPECT_EQ(ReadFile(m_Root / "two.txt"), "two");
}

TEST_F(ZipArchiveReaderTest, ADuplicateNameIsListedOnceAndExtractsTheFirstEntry)
{
    const fs::path zipFile = WriteArchive({{"tree.glb", "first"}, {"rock.glb", "rock"}, {"tree.glb", "second"}});

    std::string error;
    std::optional<ZipArchiveReader> archive = ZipArchiveReader::OpenFile(zipFile, &error);
    ASSERT_TRUE(archive) << error;
    EXPECT_EQ(archive->EntryNames(), (std::vector<std::string>{"tree.glb", "rock.glb"}));

    const std::vector<ZipEntryExtraction> chosen = {{"tree.glb", m_Root / "tree.glb"}};
    std::vector<ZipEntryFailure> failures;
    EXPECT_TRUE(archive->ExtractEntries(chosen, failures));
    EXPECT_EQ(ReadFile(m_Root / "tree.glb"), "first");
}

// The checksum is checked after the whole entry is written, so a bad checksum fails an entry
// the way a full disk does: after part of the file exists.
TEST_F(ZipArchiveReaderTest, AFailedEntryLeavesNoPartialFileAndKeepsTheOldOne)
{
    std::string bytes = ReadFile(WriteArchive({{"tree.glb", std::string(size_t{1} << 18, 'g')}, {"rock.glb", "rock"}}));
    const size_t central = CentralHeaderAt(bytes, 0);
    WriteLittleEndian(bytes, central + kCentralCrcOffset, 4, ~ReadLittleEndian(bytes, central + kCentralCrcOffset, 4));
    const fs::path output = m_Root / "out";
    fs::create_directories(output / "new");
    fs::create_directories(output / "kept");
    WriteFile(output / "kept" / "tree.glb", "an older tree");

    std::string error;
    std::optional<ZipArchiveReader> archive = ZipArchiveReader::OpenMemory(std::as_bytes(std::span(bytes)), &error);
    ASSERT_TRUE(archive) << error;
    const std::vector<ZipEntryExtraction> chosen = {{"tree.glb", output / "new" / "tree.glb"},
                                                    {"tree.glb", output / "kept" / "tree.glb"}};
    std::vector<ZipEntryFailure> failures;
    EXPECT_FALSE(archive->ExtractEntries(chosen, failures));
    EXPECT_EQ(failures.size(), 2u);

    EXPECT_EQ(CountFiles(output / "new"), 0u);
    EXPECT_EQ(CountFiles(output / "kept"), 1u);
    EXPECT_EQ(ReadFile(output / "kept" / "tree.glb"), "an older tree");
}

// The reader copies the names when it opens and reads the caller's bytes only inside
// ExtractEntries, so EntryNames and the destructor are safe after the bytes change or go.
TEST_F(ZipArchiveReaderTest, AMemoryArchiveReadsTheCallersBytesOnlyToExtract)
{
    const std::string written = ReadFile(WriteArchive({{"Models/tree.glb", "glb"}}));
    std::vector<std::byte> bytes(written.size());
    std::memcpy(bytes.data(), written.data(), written.size());

    std::string error;
    std::optional<ZipArchiveReader> archive = ZipArchiveReader::OpenMemory(bytes, &error);
    ASSERT_TRUE(archive) << error;
    constexpr std::byte kPoison{0xDD};
    std::fill(bytes.begin(), bytes.end(), kPoison);
    EXPECT_EQ(archive->EntryNames(), (std::vector<std::string>{"Models/tree.glb"}));

    const std::vector<ZipEntryExtraction> chosen = {{"Models/tree.glb", m_Root / "tree.glb"}};
    std::vector<ZipEntryFailure> failures;
    EXPECT_FALSE(archive->ExtractEntries(chosen, failures));
    EXPECT_FALSE(fs::exists(m_Root / "tree.glb"));

    bytes.clear();
    bytes.shrink_to_fit();
    EXPECT_EQ(archive->EntryNames().size(), 1u);
    archive.reset();
}

// The example of docs/packages/zip-archives.mdx: the files of one folder of a pack, into a
// folder of the project, under the same names.
TEST_F(ZipArchiveReaderTest, TheDocumentedImportCopiesOneFolderOfAPack)
{
    const fs::path zipFile = WriteArchive({{"Models/GLB format/tree.glb", "tree"},
                                           {"Models/GLB format/Rocks/rock.glb", "rock"},
                                           {"Models/GLB format/caf\xC3\xA9.glb", "cafe"},
                                           {"Models/FBX format/tree.fbx", "fbx"},
                                           {"License.txt", "license"}});
    const fs::path assetsFolder = m_Root / "Assets";

    std::string error;
    std::optional<ZipArchiveReader> archive = ZipArchiveReader::OpenFile(zipFile, &error);
    ASSERT_TRUE(archive) << error;
    const std::string folder = "Models/GLB format/";
    std::vector<ZipEntryExtraction> chosen;
    for (const std::string& name : archive->EntryNames())
    {
        if (name.starts_with(folder))
            chosen.push_back({name, assetsFolder / "Pack" / PathFromUtf8(name.substr(folder.size()))});
    }
    std::vector<ZipEntryFailure> failures;
    EXPECT_TRUE(archive->ExtractEntries(chosen, failures));

    EXPECT_EQ(ReadFile(assetsFolder / "Pack" / "tree.glb"), "tree");
    EXPECT_EQ(ReadFile(assetsFolder / "Pack" / "Rocks" / "rock.glb"), "rock");
    EXPECT_EQ(ReadFile(assetsFolder / "Pack" / fs::path(u8"caf\u00E9.glb")), "cafe");
    EXPECT_EQ(CountFiles(assetsFolder / "Pack"), 3u);
}

// An entry's bytes run from its local header past its name and extra field to the end of its
// data, so an entry hidden in another's extra field shares that entry's bytes.
TEST_F(ZipArchiveReaderTest, AnEntryHiddenInAnotherEntrysExtraFieldIsLeftOut)
{
    const std::string bytes = ArchiveWithAnEntryHiddenInAnExtraField();

    std::string error;
    std::optional<ZipArchiveReader> archive = ZipArchiveReader::OpenMemory(std::as_bytes(std::span(bytes)), &error);
    ASSERT_TRUE(archive) << error;
    EXPECT_TRUE(archive->EntryNames().empty());
}

TEST_F(ZipArchiveReaderTest, AnEncryptedEntryIsLeftOut)
{
    std::string bytes = ReadFile(WriteArchive({{"secret.txt", std::string(100, 's')}, {"plain.txt", "plain"}}));
    const size_t central = CentralHeaderAt(bytes, 0);
    const size_t local = ReadLittleEndian(bytes, central + kCentralLocalHeaderOffset, 4);
    WriteLittleEndian(bytes, local + kLocalFlagsOffset, 2, kEncryptedFlag);
    WriteLittleEndian(bytes, central + kCentralFlagsOffset, 2, kEncryptedFlag);

    std::string error;
    std::optional<ZipArchiveReader> archive = ZipArchiveReader::OpenMemory(std::as_bytes(std::span(bytes)), &error);
    ASSERT_TRUE(archive) << error;
    EXPECT_EQ(archive->EntryNames(), (std::vector<std::string>{"plain.txt"}));
}

TEST_F(ZipArchiveReaderTest, AnEntryWithACompressionMethodTheReaderDoesNotSupportIsLeftOut)
{
    std::string bytes = ReadFile(WriteArchive({{"packed.txt", std::string(100, 'p')}, {"plain.txt", "plain"}}));
    const size_t central = CentralHeaderAt(bytes, 0);
    const size_t local = ReadLittleEndian(bytes, central + kCentralLocalHeaderOffset, 4);
    WriteLittleEndian(bytes, local + kLocalMethodOffset, 2, kLzmaMethod);
    WriteLittleEndian(bytes, central + kCentralMethodOffset, 2, kLzmaMethod);

    std::string error;
    std::optional<ZipArchiveReader> archive = ZipArchiveReader::OpenMemory(std::as_bytes(std::span(bytes)), &error);
    ASSERT_TRUE(archive) << error;
    EXPECT_EQ(archive->EntryNames(), (std::vector<std::string>{"plain.txt"}));
}

// Windows opens a device for a reserved name with any extension, in any case, and drops a
// trailing dot or space from a name; such names are unsafe on every platform.
TEST_F(ZipArchiveReaderTest, LeavesOutDeviceNamesAndNamesWindowsTrims)
{
    const fs::path zipFile = WriteArchive({{"CON", "x"},
                                           {"aux.txt", "x"},
                                           {"COM1.txt", "x"},
                                           {"lpt1", "x"},
                                           {"Models/nul.glb", "x"},
                                           {"prn/tree.glb", "x"},
                                           {"tree.", "x"},
                                           {"rock ", "x"},
                                           {"CONIN$", "x"},
                                           {"conout$.txt", "x"},
                                           {"COM\xC2\xB9", "x"},     // "COM¹"
                                           {"lpt\xC2\xB3.log", "x"}, // "lpt³.log"
                                           {"console.txt", "kept"},
                                           {"Models/com.glb", "kept"}});

    std::string error;
    const std::optional<ZipArchiveReader> archive = ZipArchiveReader::OpenFile(zipFile, &error);
    ASSERT_TRUE(archive) << error;
    EXPECT_EQ(archive->EntryNames(), (std::vector<std::string>{"console.txt", "Models/com.glb"}));
}

// An empty segment, as in a doubled '/', names no file or folder of its own, so the name is kept.
TEST_F(ZipArchiveReaderTest, KeepsANameWithAnEmptySegment)
{
    const fs::path zipFile = WriteArchive({{"Models//tree.glb", "glb"}});

    std::string error;
    const std::optional<ZipArchiveReader> archive = ZipArchiveReader::OpenFile(zipFile, &error);
    ASSERT_TRUE(archive) << error;
    EXPECT_EQ(archive->EntryNames(), (std::vector<std::string>{"Models//tree.glb"}));
}

// Windows and macOS find a file by its name in any case, and macOS in either Unicode form of an
// accented letter, so a name that differs from an earlier one only in these ways would
// overwrite that entry's file.
TEST_F(ZipArchiveReaderTest, ANameThatDiffersOnlyInCaseOrUnicodeFormIsARepeat)
{
    const fs::path zipFile = WriteArchive({{"Tree.glb", "upper"},
                                           {"Models/a.glb", "a"},
                                           {"tree.glb", "lower"},
                                           {"\xC3\x84rger.txt", "upper"}, // "Ärger.txt"
                                           {"\xC3\xA4rger.txt", "lower"}, // "ärger.txt"
                                           {"caf\xC3\xA9.txt", "composed"},
                                           {"cafe\xCC\x81.txt", "decomposed"}});

    std::string error;
    std::optional<ZipArchiveReader> archive = ZipArchiveReader::OpenFile(zipFile, &error);
    ASSERT_TRUE(archive) << error;
    EXPECT_EQ(archive->EntryNames(),
              (std::vector<std::string>{"Tree.glb", "Models/a.glb", "\xC3\x84rger.txt", "caf\xC3\xA9.txt"}));
}

TEST_F(ZipArchiveReaderTest, AnEntryDeclaringLessThanItsDataFailsAndLeavesNoFile)
{
    std::string bytes = ReadFile(WriteArchive({{"small.bin", std::string(size_t{1} << 20, '\0')}}));
    const size_t central = CentralHeaderAt(bytes, 0);
    const size_t local = ReadLittleEndian(bytes, central + kCentralLocalHeaderOffset, 4);
    WriteLittleEndian(bytes, central + kCentralUncompressedSizeOffset, 4, 100);
    WriteLittleEndian(bytes, local + kLocalUncompressedSizeOffset, 4, 100);

    std::string error;
    std::optional<ZipArchiveReader> archive = ZipArchiveReader::OpenMemory(std::as_bytes(std::span(bytes)), &error);
    ASSERT_TRUE(archive) << error;
    std::vector<ZipEntryFailure> failures;
    EXPECT_FALSE(archive->ExtractEntries(std::vector<ZipEntryExtraction>{{"small.bin", m_Root / "out" / "small.bin"}},
                                         failures));
    EXPECT_EQ(CountFiles(m_Root / "out"), 0u);
}

// An entry that declares more than the default limit of one extraction is not written. The
// limit is checked against the declared size before any byte is decompressed.
TEST_F(ZipArchiveReaderTest, AnExtractionPastTheDefaultLimitWritesNothing)
{
    std::string bytes = ReadFile(WriteArchive({{"big.bin", std::string(1000, '\0')}}));
    constexpr uint32_t kDeclaredSize = uint32_t{600} << 20;
    SetDeclaredSizes(bytes, kDeclaredSize);

    std::string error;
    std::optional<ZipArchiveReader> archive = ZipArchiveReader::OpenMemory(std::as_bytes(std::span(bytes)), &error);
    ASSERT_TRUE(archive) << error;
    std::vector<ZipEntryFailure> failures;
    EXPECT_FALSE(
        archive->ExtractEntries(std::vector<ZipEntryExtraction>{{"big.bin", m_Root / "out" / "big.bin"}}, failures));
    ASSERT_EQ(failures.size(), 1u);
    EXPECT_NE(failures[0].Message.find("limit"), std::string::npos) << failures[0].Message;
    EXPECT_TRUE(failures[0].PastSizeLimit);
    EXPECT_FALSE(fs::exists(m_Root / "out"));
}

// The limit counts every entry one call extracts, before any is written: past it, no entry is
// written, so the limit never leaves a pack half extracted. Each entry's message gives the total
// and the limit.
TEST_F(ZipArchiveReaderTest, AnExtractionPastACallersLimitWritesNoEntry)
{
    const std::string megabyte(size_t{1} << 20, '\0');
    const fs::path zipFile = WriteArchive({{"a.bin", megabyte}, {"b.bin", megabyte}, {"small.txt", "small"}});

    std::string error;
    std::optional<ZipArchiveReader> archive = ZipArchiveReader::OpenFile(zipFile, &error);
    ASSERT_TRUE(archive) << error;
    const fs::path output = m_Root / "out";
    const std::vector<ZipEntryExtraction> chosen = {
        {"a.bin", output / "a.bin"}, {"b.bin", output / "b.bin"}, {"small.txt", output / "small.txt"}};
    std::vector<ZipEntryFailure> failures;
    EXPECT_FALSE(archive->ExtractEntries(chosen, failures, megabyte.size() + 1024));
    ASSERT_EQ(failures.size(), 3u);
    for (const ZipEntryFailure& failure : failures)
    {
        EXPECT_NE(failure.Message.find("2.0 MiB"), std::string::npos) << failure.Message;
        EXPECT_NE(failure.Message.find("1.0 MiB"), std::string::npos) << failure.Message;
        EXPECT_TRUE(failure.PastSizeLimit) << failure.EntryName;
    }
    EXPECT_FALSE(fs::exists(output));

    failures.clear();
    EXPECT_TRUE(archive->ExtractEntries(chosen, failures, 2 * megabyte.size() + 1024));
    EXPECT_EQ(ReadFile(output / "small.txt"), "small");
}

// A write failure's message names the destination in UTF-8, so a name the active code page on
// Windows cannot spell (日本 under code page 1252) still gives a message, with one period.
TEST_F(ZipArchiveReaderTest, AWriteFailureNamesADestinationOutsideTheCodePage)
{
    std::string error;
    std::optional<ZipArchiveReader> archive = ZipArchiveReader::OpenFile(WriteArchive({{"tree.glb", "tree"}}), &error);
    ASSERT_TRUE(archive) << error;
    WriteFile(m_Root / "blocker", "a file where the folder would be");
    const fs::path destination = m_Root / "blocker" / fs::path(u8"\u65E5\u672C") / "tree.glb";

    std::vector<ZipEntryFailure> failures;
    bool extracted = true;
    EXPECT_NO_THROW(
        extracted = archive->ExtractEntries(std::vector<ZipEntryExtraction>{{"tree.glb", destination}}, failures));
    EXPECT_FALSE(extracted);
    ASSERT_EQ(failures.size(), 1u);
    const std::string& message = failures[0].Message;
    EXPECT_NE(message.find("blocker/\xE6\x97\xA5\xE6\x9C\xAC/tree.glb"), std::string::npos) << message;
    EXPECT_FALSE(message.ends_with("..")) << message;
}
