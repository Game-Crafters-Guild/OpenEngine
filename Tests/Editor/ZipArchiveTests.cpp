// The editor's zip reader as package modules see it through EditorSDK: listing an
// archive's files, extracting a chosen set of them, and refusing entry names that
// would write outside the destination.

#include <gtest/gtest.h>

#include "Editor/ZipArchive.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <miniz.h>

namespace fs = std::filesystem;
using GameEngine::Editor::ExtractZipArchive;
using GameEngine::Editor::ExtractZipEntries;
using GameEngine::Editor::ListZipEntries;
using GameEngine::Editor::ZipEntryExtraction;
using GameEngine::Editor::ZipEntryFailure;

namespace
{

// An archive entry: a name ending in '/' is a folder entry.
using ArchiveEntry = std::pair<std::string, std::string>;

class ZipArchiveTest : public ::testing::Test
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

    fs::path WriteArchive(const std::vector<ArchiveEntry>& entries)
    {
        const fs::path zipFile = m_Root / "archive.zip";
        mz_zip_archive archive{};
        EXPECT_TRUE(mz_zip_writer_init_file(&archive, zipFile.string().c_str(), 0));
        for (const auto& [name, contents] : entries)
        {
            EXPECT_TRUE(mz_zip_writer_add_mem(&archive, name.c_str(), contents.data(), contents.size(),
                                              MZ_DEFAULT_COMPRESSION))
                << name;
        }
        EXPECT_TRUE(mz_zip_writer_finalize_archive(&archive));
        EXPECT_TRUE(mz_zip_writer_end(&archive));
        return zipFile;
    }

    // miniz refuses to write a name that starts with '/', so the name is written with
    // a placeholder first byte and patched in the local header and the central directory.
    fs::path WriteArchiveWithAbsoluteEntry(const std::string& placeholderName,
                                           const std::vector<ArchiveEntry>& otherEntries)
    {
        std::vector<ArchiveEntry> entries = otherEntries;
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

TEST_F(ZipArchiveTest, ListsFilesInArchiveOrderWithoutFolders)
{
    const fs::path zipFile = WriteArchive({{"readme.txt", "read me"},
                                           {"Models/", ""},
                                           {"Models/GLB format/tree.glb", "glb"},
                                           {"Audio/click.ogg", "ogg"}});

    std::vector<std::string> names;
    std::string error;
    ASSERT_TRUE(ListZipEntries(zipFile, names, &error)) << error;
    EXPECT_EQ(names, (std::vector<std::string>{"readme.txt", "Models/GLB format/tree.glb",
                                               "Audio/click.ogg"}));
}

TEST_F(ZipArchiveTest, ListLeavesOutUnsafeNames)
{
    const fs::path zipFile = WriteArchiveWithAbsoluteEntry(
        "_absolute.txt", {{"../escape.txt", "x"},
                          {"Models/../../escape.txt", "x"},
                          {"./here.txt", "x"},
                          {"Models\\windows.txt", "x"},
                          {"C:/drive.txt", "x"},
                          {"Models/safe.txt", "safe"}});

    std::vector<std::string> names;
    std::string error;
    ASSERT_TRUE(ListZipEntries(zipFile, names, &error)) << error;
    EXPECT_EQ(names, (std::vector<std::string>{"Models/safe.txt"}));
}

TEST_F(ZipArchiveTest, ListReportsAFileThatIsNotAnArchive)
{
    const fs::path notZip = m_Root / "download.zip";
    std::ofstream(notZip) << "not a zip";

    std::vector<std::string> names;
    std::string error;
    EXPECT_FALSE(ListZipEntries(notZip, names, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(ListZipEntries(m_Root / "missing.zip", names, &error));
    EXPECT_NE(error.find("missing.zip"), std::string::npos) << error;
}

TEST_F(ZipArchiveTest, ExtractsOnlyTheChosenEntriesToTheirDestinations)
{
    const fs::path zipFile = WriteArchive({{"Models/GLB format/tree.glb", "glb"},
                                           {"Models/FBX format/tree.fbx", "fbx"},
                                           {"License.txt", "license"}});
    const fs::path output = m_Root / "Project" / "Assets" / "Kenney";
    fs::create_directories(output);
    std::ofstream(output / "License.txt") << "an older license that is replaced";

    const std::vector<ZipEntryExtraction> chosen = {
        {"Models/GLB format/tree.glb", output / "Models" / "tree.glb"},
        {"License.txt", output / "License.txt"}};
    std::vector<ZipEntryFailure> failures;
    std::string error;
    ASSERT_TRUE(ExtractZipEntries(zipFile, chosen, failures, &error)) << error;
    EXPECT_TRUE(failures.empty());

    EXPECT_EQ(ReadFile(output / "Models" / "tree.glb"), "glb");
    EXPECT_EQ(ReadFile(output / "License.txt"), "license");
    EXPECT_FALSE(fs::exists(output / "Models" / "FBX format"));
    EXPECT_FALSE(fs::exists(output / "Models" / "tree.fbx"));
}

TEST_F(ZipArchiveTest, ExtractReportsEachFailedEntryAndExtractsTheRest)
{
    const fs::path zipFile = WriteArchive({{"Models/", ""},
                                           {"Models/tree.glb", "glb"},
                                           {"../escape.txt", "escaped"},
                                           {"Audio/click.ogg", "ogg"}});
    const fs::path output = m_Root / "out";
    fs::create_directories(output / "click.ogg");

    const std::vector<ZipEntryExtraction> chosen = {
        {"Models/missing.glb", output / "missing.glb"},
        {"Models/", output / "Models"},
        {"../escape.txt", output / "escape.txt"},
        {"Audio/click.ogg", output / "click.ogg"},
        {"Models/tree.glb", output / "tree.glb"}};
    std::vector<ZipEntryFailure> failures;
    std::string error;
    EXPECT_FALSE(ExtractZipEntries(zipFile, chosen, failures, &error));
    EXPECT_TRUE(error.empty()) << error;

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

TEST_F(ZipArchiveTest, ExtractReportsAnUnreadableArchiveWithoutEntryFailures)
{
    const fs::path notZip = m_Root / "download.zip";
    std::ofstream(notZip) << "not a zip";

    const std::vector<ZipEntryExtraction> chosen = {{"readme.txt", m_Root / "readme.txt"}};
    std::vector<ZipEntryFailure> failures;
    std::string error;
    EXPECT_FALSE(ExtractZipEntries(notZip, chosen, failures, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(failures.empty());
}

TEST_F(ZipArchiveTest, WholeArchiveExtractionSkipsUnsafeEntries)
{
    const fs::path zipFile = WriteArchiveWithAbsoluteEntry(
        "_absolute.txt", {{"Assets/scene.json", "scene"}, {"../escape.txt", "escaped"}});
    const fs::path output = m_Root / "project";

    std::string error;
    ASSERT_TRUE(ExtractZipArchive(zipFile, output, "", &error)) << error;
    EXPECT_EQ(ReadFile(output / "Assets" / "scene.json"), "scene");
    EXPECT_FALSE(fs::exists(m_Root / "escape.txt"));
    EXPECT_FALSE(fs::exists(output / "escape.txt"));
}

TEST_F(ZipArchiveTest, InnerRootExtractionStripsTheFolderPrefix)
{
    const fs::path zipFile = WriteArchive({{"repo-main/", ""},
                                           {"repo-main/project/Assets/scene.json", "scene"},
                                           {"repo-main/README.md", "readme"}});
    const fs::path output = m_Root / "project";

    std::string error;
    ASSERT_TRUE(ExtractZipArchive(zipFile, output, "repo-main/project", &error)) << error;
    EXPECT_EQ(ReadFile(output / "Assets" / "scene.json"), "scene");
    EXPECT_FALSE(fs::exists(output / "README.md"));

    EXPECT_FALSE(ExtractZipArchive(zipFile, m_Root / "other", "repo-main/absent", &error));
    EXPECT_NE(error.find("repo-main/absent"), std::string::npos) << error;
}
