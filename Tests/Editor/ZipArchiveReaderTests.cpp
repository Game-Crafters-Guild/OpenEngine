// The editor's zip reader as package modules see it through EditorSDK: opening an
// archive on disk or in memory, listing its files, extracting a chosen set of them,
// and refusing entry names that would write outside the destination.

#include <gtest/gtest.h>

#include "Editor/ZipArchiveReader.h"
#include "TestZipArchive.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using GameEngine::Editor::ZipArchiveReader;
using GameEngine::Editor::ZipEntryExtraction;
using GameEngine::Editor::ZipEntryFailure;

namespace
{

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

    // miniz refuses to write a name that starts with '/', so the name is written with
    // a placeholder first byte and patched in the local header and the central directory.
    fs::path WriteArchiveWithAbsoluteEntry(const std::string& placeholderName,
                                           const std::vector<TestZipEntry>& otherEntries)
    {
        std::vector<TestZipEntry> entries = otherEntries;
        entries.emplace_back(placeholderName, "absolute");
        const fs::path zipFile = WriteArchive(entries);

        std::string bytes;
        {
            std::ifstream in(zipFile, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        size_t patched = 0;
        for (size_t at = bytes.find(placeholderName); at != std::string::npos;
             at = bytes.find(placeholderName, at + 1))
        {
            bytes[at] = '/';
            ++patched;
        }
        EXPECT_EQ(patched, 2u);
        std::ofstream out(zipFile, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return zipFile;
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
    const fs::path zipFile = WriteArchiveWithAbsoluteEntry(
        "_absolute.txt", {{"../escape.txt", "x"},
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
        EXPECT_FALSE(failure.Message.empty()) << failure.EntryName;
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
