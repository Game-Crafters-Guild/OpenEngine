// A project acquisition is a job of its "Project acquisition" channel: a clone, a
// download or a template copy holds no compute worker. A zip source extracts the
// archive, or only the folder its manifest path names.

#include <gtest/gtest.h>

#include "JobSystem/WorkStealingThreadPool.h"
#include "Projects/ProjectAcquisition.h"
#include "ScopedEditorUserDataRoot.h"
#include "TestZipArchive.h"
#include "Types/StringUtils.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using GameEngine::Editor::ProjectAcquireRequest;
using GameEngine::Editor::ProjectAcquireState;
using GameEngine::Editor::ProjectAcquireStatus;
using GameEngine::Editor::ProjectAcquisition;
using GameEngine::Editor::ProjectSourceType;

namespace
{

constexpr std::chrono::seconds kAcquisitionTimeout{10};
constexpr std::chrono::milliseconds kPollInterval{1};

// Polls until the acquisition leaves Running or kAcquisitionTimeout passes.
ProjectAcquireStatus WaitForAcquisition(ProjectAcquisition& acquisition)
{
    ProjectAcquireStatus status;
    const auto deadline = std::chrono::steady_clock::now() + kAcquisitionTimeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (acquisition.Poll(status) && status.State != ProjectAcquireState::Running)
            break;
        std::this_thread::sleep_for(kPollInterval);
    }
    return status;
}

// Acquires the archive's innerRoot folder as root/projects/Acquired, through the
// Import -> Archive path (a .zip already on disk).
ProjectAcquireStatus AcquireLocalZip(const fs::path& root, const fs::path& zipFile,
                                     const std::string& innerRoot)
{
    JobSystem::WorkStealingThreadPool pool(1);
    ProjectAcquisition acquisition(pool);
    ProjectAcquireRequest request;
    request.Entry.Id = "archive";
    request.Entry.Name = "Archive";
    request.Entry.Source.Type = ProjectSourceType::Zip;
    request.Entry.Source.Path = innerRoot;
    request.LocalArchivePath = zipFile;
    request.Destination = root / "projects" / "Acquired";
    request.DisplayName = "Acquired";
    if (!acquisition.Start(request))
        return {};
    return WaitForAcquisition(acquisition);
}

// True when path::string() throws for file, as on Windows when the active code page cannot spell
// its name.
bool PathStringThrows(const fs::path& file)
{
    try
    {
        (void)file.string();
    }
    catch (const std::exception&)
    {
        return true;
    }
    return false;
}

fs::path MakeScratchRoot()
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() / ("ge_acquisition_" + std::to_string(stamp));
    fs::create_directories(root / "user");
    return root;
}

} // namespace

// With the pool's only compute worker held, a template copy still completes.
TEST(ProjectAcquisition, AnAcquisitionHoldsNoComputeWorker)
{
    const fs::path root = MakeScratchRoot();
    fs::create_directories(root / "templates" / "empty" / "Assets");
    {
        std::ofstream file(root / "templates" / "empty" / "Assets" / "readme.txt");
        file << "template";
    }
    const ScopedEditorUserDataRoot userDataRoot(root / "user");

    JobSystem::WorkStealingThreadPool pool(1);
    std::promise<void> release;
    std::promise<void> workerHeld;
    JobSystem::TaskHandle held = pool.Submit([&workerHeld, released = release.get_future().share()]() {
        workerHeld.set_value();
        released.wait();
    });
    workerHeld.get_future().wait();

    ProjectAcquireStatus status;
    {
        ProjectAcquisition acquisition(pool);
        ProjectAcquireRequest request;
        request.Entry.Id = "empty";
        request.Entry.Name = "Empty";
        request.Entry.Source.Type = ProjectSourceType::Local;
        request.Entry.Source.Path = "empty";
        request.CatalogRoot = root / "templates";
        request.Destination = root / "projects" / "Acquired";
        request.DisplayName = "Acquired";
        ASSERT_TRUE(acquisition.Start(request));
        status = WaitForAcquisition(acquisition);
        release.set_value();
        held.Wait();
    }
    EXPECT_EQ(status.State, ProjectAcquireState::Succeeded)
        << "the acquisition waited for the held compute worker (" << status.Error << ")";
    EXPECT_TRUE(fs::exists(root / "projects" / "Acquired" / "Assets" / "readme.txt"));

    std::error_code ec;
    fs::remove_all(root, ec);
}

// Only the manifest's folder is extracted, with its prefix stripped; an unsafe entry is skipped.
TEST(ProjectAcquisition, AZipSourceExtractsItsInnerFolder)
{
    const fs::path root = MakeScratchRoot();
    const ScopedEditorUserDataRoot userDataRoot(root / "user");
    const fs::path zipFile = root / "project.zip";
    ASSERT_TRUE(WriteTestZip(zipFile, {{"repo-main/", ""},
                                       {"repo-main/project/Assets/readme.txt", "project"},
                                       {"repo-main/project/../../escape.txt", "escaped"},
                                       {"repo-main/README.md", "repository readme"}}));

    const ProjectAcquireStatus status = AcquireLocalZip(root, zipFile, "repo-main/project");
    EXPECT_EQ(status.State, ProjectAcquireState::Succeeded) << status.Error;
    EXPECT_TRUE(fs::exists(root / "projects" / "Acquired" / "Assets" / "readme.txt"));
    EXPECT_FALSE(fs::exists(root / "projects" / "Acquired" / "README.md"));
    EXPECT_FALSE(fs::exists(root / "projects" / "escape.txt"));
    EXPECT_FALSE(fs::exists(root / "escape.txt"));

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(ProjectAcquisition, AZipSourceWithoutItsInnerFolderFails)
{
    const fs::path root = MakeScratchRoot();
    const ScopedEditorUserDataRoot userDataRoot(root / "user");
    const fs::path zipFile = root / "project.zip";
    ASSERT_TRUE(WriteTestZip(zipFile, {{"repo-main/README.md", "repository readme"}}));

    const ProjectAcquireStatus status = AcquireLocalZip(root, zipFile, "repo-main/project");
    EXPECT_EQ(status.State, ProjectAcquireState::Failed);
    EXPECT_NE(status.Error.find("repo-main/project"), std::string::npos) << status.Error;

    std::error_code ec;
    fs::remove_all(root, ec);
}

// Entry names are UTF-8; the extracted files have the same names on every platform.
TEST(ProjectAcquisition, AZipSourceKeepsNonAsciiNames)
{
    const fs::path root = MakeScratchRoot();
    const ScopedEditorUserDataRoot userDataRoot(root / "user");
    const fs::path zipFile = root / "project.zip";
    ASSERT_TRUE(WriteTestZip(zipFile, {{"readme.txt", "readme"}, {"Assets/Gr\xC3\xB6\xC3\x9F" "e/caf\xC3\xA9.txt", "cafe"}}));

    const ProjectAcquireStatus status = AcquireLocalZip(root, zipFile, "");
    EXPECT_EQ(status.State, ProjectAcquireState::Succeeded) << status.Error;
    const fs::path assets = root / "projects" / "Acquired" / "Assets";
    EXPECT_TRUE(fs::exists(assets / fs::path(u8"Gr\u00F6\u00DFe") / fs::path(u8"caf\u00E9.txt")));

    std::error_code ec;
    fs::remove_all(root, ec);
}

// A project archive whose files are larger than the import limit writes nothing, and the
// notice says what to do instead.
TEST(ProjectAcquisition, AZipSourcePastTheImportLimitWritesNothingAndSaysWhatToDo)
{
    const fs::path root = MakeScratchRoot();
    const ScopedEditorUserDataRoot userDataRoot(root / "user");
    const fs::path zipFile = root / "project.zip";
    ASSERT_TRUE(WriteTestZip(zipFile, {{"a.bin", "a"}, {"b.bin", "b"}, {"c.bin", "c"}}));
    std::string bytes;
    {
        std::ifstream in(zipFile, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    // Three entries of almost 4 GiB each declare more than the 8 GiB limit together.
    SetDeclaredSizes(bytes, 0xFFFF0000u);
    std::ofstream(zipFile, std::ios::binary | std::ios::trunc).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));

    const ProjectAcquireStatus status = AcquireLocalZip(root, zipFile, "");
    EXPECT_EQ(status.State, ProjectAcquireState::Failed);
    EXPECT_EQ(status.Error, "The project is larger than 8 GiB, the most an archive import writes. Extract the "
                            "archive yourself, then import the folder from the Folder tab.");
    EXPECT_FALSE(fs::exists(root / "projects" / "Acquired" / "a.bin"));

    std::error_code ec;
    fs::remove_all(root, ec);
}

// An older zip tool writes a name in a code page such as 437 ("caf\x82.txt" for café.txt). The
// import leaves that file out and extracts the rest.
TEST(ProjectAcquisition, AZipSourceLeavesOutANameThatIsNotUtf8)
{
    const fs::path root = MakeScratchRoot();
    const ScopedEditorUserDataRoot userDataRoot(root / "user");
    const fs::path zipFile = root / "project.zip";
    ASSERT_TRUE(WriteTestZip(zipFile, {{"readme.txt", "readme"}, {"Assets/caf_.txt", "cafe"}}));
    std::string bytes;
    {
        std::ifstream in(zipFile, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    bytes = GameEngine::ReplaceAll(std::move(bytes), "caf_.txt", "caf\x82.txt");
    std::ofstream(zipFile, std::ios::binary | std::ios::trunc).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));

    const ProjectAcquireStatus status = AcquireLocalZip(root, zipFile, "");
    EXPECT_EQ(status.State, ProjectAcquireState::Succeeded) << status.Error;
    const fs::path project = root / "projects" / "Acquired";
    EXPECT_TRUE(fs::exists(project / "readme.txt"));
    std::error_code ec;
    EXPECT_TRUE(fs::is_empty(project / "Assets", ec));

    fs::remove_all(root, ec);
}

// An exception inside the acquisition ends it as failed, with the exception's message, and
// removes the folder it made. The template copy throws when path::string() cannot spell a name.
TEST(ProjectAcquisition, AnExceptionEndsTheAcquisitionWithItsMessage)
{
    const fs::path emojiFile(u8"\U0001F600.txt");
    if (!PathStringThrows(emojiFile))
        GTEST_SKIP() << "path::string() spells an emoji name here (not Windows, or a UTF-8 code page), so nothing throws";
    const fs::path root = MakeScratchRoot();
    fs::create_directories(root / "templates" / "emoji");
    std::ofstream(root / "templates" / "emoji" / emojiFile) << "template";
    const ScopedEditorUserDataRoot userDataRoot(root / "user");

    JobSystem::WorkStealingThreadPool pool(1);
    ProjectAcquisition acquisition(pool);
    ProjectAcquireRequest request;
    request.Entry.Id = "emoji";
    request.Entry.Name = "Emoji";
    request.Entry.Source.Type = ProjectSourceType::Local;
    request.Entry.Source.Path = "emoji";
    request.CatalogRoot = root / "templates";
    request.Destination = root / "projects" / "Acquired";
    request.DisplayName = "Acquired";
    ASSERT_TRUE(acquisition.Start(request));
    const ProjectAcquireStatus status = WaitForAcquisition(acquisition);
    ASSERT_EQ(status.State, ProjectAcquireState::Failed) << status.Error;
    EXPECT_TRUE(status.Error.starts_with("The project could not be created: ")) << status.Error;
    EXPECT_FALSE(fs::exists(root / "projects" / "Acquired"));

    std::error_code ec;
    fs::remove_all(root, ec);
}
