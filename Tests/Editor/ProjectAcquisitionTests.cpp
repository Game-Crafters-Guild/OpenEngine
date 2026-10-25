// A project acquisition is a job of its "Project acquisition" channel: a clone, a
// download or a template copy holds no compute worker. A zip source extracts the
// archive, or only the folder its manifest path names.

#include <gtest/gtest.h>

#include "JobSystem/WorkStealingThreadPool.h"
#include "Projects/ProjectAcquisition.h"
#include "ScopedEditorUserDataRoot.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <miniz.h>

namespace fs = std::filesystem;
using GameEngine::Editor::ProjectAcquireRequest;
using GameEngine::Editor::ProjectAcquireState;
using GameEngine::Editor::ProjectAcquireStatus;
using GameEngine::Editor::ProjectAcquisition;
using GameEngine::Editor::ProjectSourceType;

namespace
{

// Writes a .zip of (name, contents) entries; a name ending in '/' is a folder entry.
void WriteZip(const fs::path& zipFile, const std::vector<std::pair<std::string, std::string>>& entries)
{
    mz_zip_archive archive{};
    ASSERT_TRUE(mz_zip_writer_init_file(&archive, zipFile.string().c_str(), 0));
    for (const auto& [name, contents] : entries)
    {
        ASSERT_TRUE(mz_zip_writer_add_mem(&archive, name.c_str(), contents.data(), contents.size(),
                                          MZ_DEFAULT_COMPRESSION));
    }
    ASSERT_TRUE(mz_zip_writer_finalize_archive(&archive));
    ASSERT_TRUE(mz_zip_writer_end(&archive));
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
    ProjectAcquireStatus status;
    if (!acquisition.Start(request))
        return status;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (acquisition.Poll(status) && status.State != ProjectAcquireState::Running)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return status;
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
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() / ("ge_acquisition_" + std::to_string(stamp));
    fs::create_directories(root / "user");
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

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (acquisition.Poll(status) && status.State != ProjectAcquireState::Running)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
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
    WriteZip(zipFile, {{"repo-main/", ""},
                       {"repo-main/project/Assets/readme.txt", "project"},
                       {"repo-main/project/../../escape.txt", "escaped"},
                       {"repo-main/README.md", "repository readme"}});

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
    WriteZip(zipFile, {{"repo-main/README.md", "repository readme"}});

    const ProjectAcquireStatus status = AcquireLocalZip(root, zipFile, "repo-main/project");
    EXPECT_EQ(status.State, ProjectAcquireState::Failed);
    EXPECT_NE(status.Error.find("repo-main/project"), std::string::npos) << status.Error;

    std::error_code ec;
    fs::remove_all(root, ec);
}
