#include <gtest/gtest.h>

#include "Assets/AssetRename.h"
#include "Assets/AssetRenameController.h"
#include "InspectorLayoutFixture.h"
#include "EditorContextMenu/DirectoryMenuEditItems.h"
#include "RecordingContextMenu.h"
#include "UndoRedo/RenameAssetCommand.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Engine/Rendering/ShaderCompileErrorLog.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Platform/ContextMenu.h"
#include "UI/Controls/Label.h"
#include "UI/StyleProperties.h"

#include "../TestTempDir.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

using namespace GameEngine::Editor;
using GameEngine::TestUtils::MakeUniqueTempDirectory;

namespace {

// The GUID Create -> Surface Shader stamps into the companion material: the
// shader's path-derived identity at creation.
constexpr const char* kPairShaderGuid = "0f6d3e4a-1b2c-4d5e-8f90-a1b2c3d4e5f6";

std::string ReadAll(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

class AssetRenameTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Dir = MakeUniqueTempDirectory("asset_rename");
        std::filesystem::create_directories(m_Dir);
    }
    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(m_Dir, ec);
    }

    std::filesystem::path Write(const char* name, const std::string& text)
    {
        const std::filesystem::path path = m_Dir / name;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
        return path;
    }

    // The pair `Create -> Surface Shader` writes: same stem, material naming the shader
    // by path and by GUID.
    std::filesystem::path WriteSurfacePair(const char* stem)
    {
        const std::string shaderName = std::string(stem) + ".glsl";
        Write((std::string(stem) + ".material").c_str(),
              "{\n  \"materialName\": \"" + std::string(stem) + "\",\n  \"surfaceShader\": \"" + shaderName +
                  "\",\n  \"surfaceShaderGuid\": \"" + std::string(kPairShaderGuid) + "\"\n}\n");
        return Write(shaderName.c_str(), "void Surface() {}\n");
    }

    std::filesystem::path m_Dir;
};

// Plan paths can arrive project-relative (the browser's form), while records
// keyed by an asset's registry path — the shader compile error log — carry the
// registry's canonical ABSOLUTE form. These tests run against a real registry
// with the fixture directory mounted as the project source, so ProjectRoot()
// resolves relative paths exactly as the live editor does. (Fixture pattern:
// Engine/Tests/AssetRenamePathTests.cpp.)
class AssetRenameRegistryTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Dir = MakeUniqueTempDirectory("asset_rename_registry");
        std::filesystem::create_directories(m_Dir);
        m_Pool = std::make_unique<JobSystem::WorkStealingThreadPool>(2);
        m_Assets = std::make_unique<GameEngine::AssetManager>();
        ASSERT_TRUE(m_Assets->Initialize(m_Pool.get()));
    }

    void TearDown() override
    {
        m_Assets.reset();
        m_Pool.reset();
        std::error_code ec;
        std::filesystem::remove_all(m_Dir, ec);
    }

    void RegisterProjectSource()
    {
        GameEngine::AssetSourceDesc source;
        source.Alias = "project"; // sets ProjectRoot(): relative lookups rebase against it
        source.Root = m_Dir;
        source.DerivedIdentity = true;
        source.RequiresScan = true;
        ASSERT_TRUE(m_Assets->RegisterSource(source));
        m_Assets->WaitForStartupScan("project");
    }

    std::filesystem::path WriteMaterial(const char* name)
    {
        const std::filesystem::path path = m_Dir / name;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "{\n  \"surfaceShader\": \"NewSurface.glsl\"\n}\n";
        return path;
    }

    std::filesystem::path m_Dir;
    std::unique_ptr<JobSystem::WorkStealingThreadPool> m_Pool;
    std::unique_ptr<GameEngine::AssetManager> m_Assets;
};

} // namespace

TEST_F(AssetRenameTests, AcceptsAnOrdinaryName)
{
    const std::filesystem::path mat = Write("Foo.material", "{}");
    const AssetRenameValidation v = ValidateAssetRename("Wood Planks_02", mat);
    EXPECT_TRUE(v.Ok);
    EXPECT_TRUE(v.Reason.empty());
}

TEST_F(AssetRenameTests, RejectsEmptyAndBlankNamesWithAReason)
{
    const std::filesystem::path mat = Write("Foo.material", "{}");
    for (const char* stem : {"", "   ", "\t"})
    {
        const AssetRenameValidation v = ValidateAssetRename(stem, mat);
        EXPECT_FALSE(v.Ok) << "'" << stem << "'";
        EXPECT_FALSE(v.Reason.empty()) << "'" << stem << "'";
    }
}

TEST_F(AssetRenameTests, RejectsSeparatorsAndCharactersNoFilesystemAccepts)
{
    const std::filesystem::path mat = Write("Foo.material", "{}");
    for (const char* stem : {"a/b", "a\\b", "a:b", "a*b", "a?b", "a\"b", "a<b", "a>b", "a|b", "a\x01b"})
    {
        const AssetRenameValidation v = ValidateAssetRename(stem, mat);
        EXPECT_FALSE(v.Ok) << "'" << stem << "'";
        EXPECT_FALSE(v.Reason.empty()) << "'" << stem << "'";
    }
}

TEST_F(AssetRenameTests, RejectsHiddenFileDotsAndTrailingDotOrSpace)
{
    const std::filesystem::path mat = Write("Foo.material", "{}");
    EXPECT_FALSE(ValidateAssetRename(".hidden", mat).Ok);
    EXPECT_FALSE(ValidateAssetRename(" lead", mat).Ok);
    EXPECT_FALSE(ValidateAssetRename("trail ", mat).Ok);
    EXPECT_FALSE(ValidateAssetRename("trail.", mat).Ok);
    EXPECT_TRUE(ValidateAssetRename("dotted.name", mat).Ok);
}

TEST_F(AssetRenameTests, RejectsWindowsReservedNamesCaseInsensitively)
{
    const std::filesystem::path mat = Write("Foo.material", "{}");
    for (const char* stem : {"CON", "con", "Nul", "COM1", "lpt9"})
    {
        const AssetRenameValidation v = ValidateAssetRename(stem, mat);
        EXPECT_FALSE(v.Ok) << stem;
        EXPECT_NE(v.Reason.find("reserved"), std::string::npos) << stem;
    }
    EXPECT_TRUE(ValidateAssetRename("CON1", mat).Ok);
    EXPECT_TRUE(ValidateAssetRename("Console", mat).Ok);
}

TEST_F(AssetRenameTests, RejectsANameAlreadyTakenBySiblingAndNamesTheFile)
{
    const std::filesystem::path mat = Write("Foo.material", "{}");
    Write("Taken.material", "{}");
    const AssetRenameValidation v = ValidateAssetRename("Taken", mat);
    EXPECT_FALSE(v.Ok);
    EXPECT_NE(v.Reason.find("Taken.material"), std::string::npos) << v.Reason;
    // A different extension is a different file: no collision.
    Write("Free.glsl", "");
    EXPECT_TRUE(ValidateAssetRename("Free", mat).Ok);
}

TEST_F(AssetRenameTests, AllowsRenamingAFileToItsOwnNameInAnotherCase)
{
    const std::filesystem::path mat = Write("Foo.material", "{}");
    EXPECT_TRUE(ValidateAssetRename("foo", mat).Ok);
    EXPECT_TRUE(ValidateAssetRename("FOO", mat).Ok);
}

TEST_F(AssetRenameTests, PairedMaterialIsFoundOnlyWhenItNamesTheShader)
{
    const std::filesystem::path paired = WriteSurfacePair("Rock");
    EXPECT_EQ(FindPairedSurfaceShaderMaterial(paired), m_Dir / "Rock.material");

    // Same stem, but the material points at some other surface: not a pair.
    Write("Loner.material", "{\n  \"surfaceShader\": \"Surfaces/standard_pbr.glsl\"\n}\n");
    const std::filesystem::path loner = Write("Loner.glsl", "");
    EXPECT_TRUE(FindPairedSurfaceShaderMaterial(loner).empty());

    // No material at all, and a non-shader asset never has a companion.
    const std::filesystem::path alone = Write("Alone.glsl", "");
    EXPECT_TRUE(FindPairedSurfaceShaderMaterial(alone).empty());
    EXPECT_TRUE(FindPairedSurfaceShaderMaterial(m_Dir / "Rock.material").empty());
}

TEST_F(AssetRenameTests, RejectsARenameWhosePairedMaterialWouldCollide)
{
    const std::filesystem::path shader = WriteSurfacePair("Rock");
    Write("Stone.material", "{}");
    const AssetRenameValidation v = ValidateAssetRename("Stone", shader);
    EXPECT_FALSE(v.Ok);
    EXPECT_NE(v.Reason.find("Stone.material"), std::string::npos) << v.Reason;
}

TEST_F(AssetRenameTests, PlanCarriesTheCompanionForASurfaceShader)
{
    const std::filesystem::path shader = WriteSurfacePair("Rock");
    const AssetRenamePlan plan = PlanAssetRename(shader, "Stone");
    EXPECT_EQ(plan.From, shader);
    EXPECT_EQ(plan.To, m_Dir / "Stone.glsl");
    ASSERT_TRUE(plan.HasCompanion());
    EXPECT_EQ(plan.CompanionFrom, m_Dir / "Rock.material");
    EXPECT_EQ(plan.CompanionTo, m_Dir / "Stone.material");

    const AssetRenamePlan solo = PlanAssetRename(m_Dir / "Rock.material", "Stone");
    EXPECT_FALSE(solo.HasCompanion());
    EXPECT_EQ(solo.To, m_Dir / "Stone.material");
}

TEST_F(AssetRenameTests, RewriteReplacesOnlyTheFilenameOfTheReference)
{
    const std::filesystem::path mat =
        Write("Foo.material", "{\n  \"surfaceShader\": \"shaders/Foo.glsl\"\n}\n");
    EXPECT_TRUE(RewriteMaterialShaderReferences(mat, "Foo.glsl", "Bar.glsl"));
    EXPECT_NE(ReadAll(mat).find("\"surfaceShader\": \"shaders/Bar.glsl\""), std::string::npos) << ReadAll(mat);

    // A reference to some other shader is left alone and reported as untouched.
    EXPECT_FALSE(RewriteMaterialShaderReferences(mat, "Foo.glsl", "Baz.glsl"));
    EXPECT_NE(ReadAll(mat).find("shaders/Bar.glsl"), std::string::npos);
}

// Under derived identity the GUID Create stamped belongs to the shader's OLD
// path: kept across the rename it would name whatever file next claims that
// path (the next Create -> Surface Shader). The rewrite drops it; the path
// carries the reference until the next save refills the GUID from it.
TEST_F(AssetRenameTests, RewriteDropsTheShaderGuidThePathSupersedes)
{
    const std::filesystem::path mat = Write(
        "Foo.material",
        "{\n  \"surfaceShader\": \"Foo.glsl\",\n  \"surfaceShaderGuid\": \"" + std::string(kPairShaderGuid) + "\"\n}\n");
    EXPECT_TRUE(RewriteMaterialShaderReferences(mat, "Foo.glsl", "Bar.glsl"));
    const std::string text = ReadAll(mat);
    EXPECT_NE(text.find("\"surfaceShader\": \"Bar.glsl\""), std::string::npos) << text;
    EXPECT_EQ(text.find("surfaceShaderGuid"), std::string::npos) << text;
}

// The inspector points the vertex-modifier pair at the surface shader's file when
// that file carries both stages, so the rename follows it there too; a vertex
// modifier naming another file is left alone, GUID included.
TEST_F(AssetRenameTests, RewriteFollowsTheVertexModifierPairToTheSameFile)
{
    const std::filesystem::path coupled = Write(
        "Foo.material",
        "{\n  \"surfaceShader\": \"Foo.glsl\",\n  \"surfaceShaderGuid\": \"" + std::string(kPairShaderGuid) +
            "\",\n  \"vertexModifier\": \"Foo.glsl\",\n  \"vertexModifierGuid\": \"" + std::string(kPairShaderGuid) +
            "\"\n}\n");
    EXPECT_TRUE(RewriteMaterialShaderReferences(coupled, "Foo.glsl", "Bar.glsl"));
    std::string text = ReadAll(coupled);
    EXPECT_NE(text.find("\"surfaceShader\": \"Bar.glsl\""), std::string::npos) << text;
    EXPECT_NE(text.find("\"vertexModifier\": \"Bar.glsl\""), std::string::npos) << text;
    EXPECT_EQ(text.find("Guid"), std::string::npos) << text;

    const std::filesystem::path separate = Write(
        "Solo.material",
        "{\n  \"surfaceShader\": \"Solo.glsl\",\n  \"vertexModifier\": \"Waves.glsl\",\n  \"vertexModifierGuid\": \"" +
            std::string(kPairShaderGuid) + "\"\n}\n");
    EXPECT_TRUE(RewriteMaterialShaderReferences(separate, "Solo.glsl", "Stone.glsl"));
    text = ReadAll(separate);
    EXPECT_NE(text.find("\"surfaceShader\": \"Stone.glsl\""), std::string::npos) << text;
    EXPECT_NE(text.find("\"vertexModifier\": \"Waves.glsl\""), std::string::npos) << text;
    EXPECT_NE(text.find("\"vertexModifierGuid\": \"" + std::string(kPairShaderGuid) + "\""), std::string::npos)
        << text;
}

TEST_F(AssetRenameTests, MaterialNameFollowsTheStemOnlyWhileItStillMatches)
{
    const std::filesystem::path mat = Write("Foo.material", "{\n  \"materialName\": \"Foo\"\n}\n");
    EXPECT_TRUE(RewriteMaterialName(mat, "Foo", "Bar"));
    EXPECT_NE(ReadAll(mat).find("\"materialName\": \"Bar\""), std::string::npos) << ReadAll(mat);

    // A hand-edited display name is the user's; a rename leaves it alone.
    const std::filesystem::path custom = Write("Custom.material", "{\n  \"materialName\": \"Shiny\"\n}\n");
    EXPECT_FALSE(RewriteMaterialName(custom, "Custom", "Other"));
    EXPECT_NE(ReadAll(custom).find("\"materialName\": \"Shiny\""), std::string::npos);
}

TEST_F(AssetRenameTests, CommandRenamesThePairRewritesTheReferenceAndUndoRestoresIt)
{
    const std::filesystem::path shader = WriteSurfacePair("Rock");
    std::vector<std::filesystem::path> renamedTo;
    RenameAssetCommand command(PlanAssetRename(shader, "Stone"), /*assets=*/nullptr,
                               [&renamedTo](const std::filesystem::path& p) { renamedTo.push_back(p); });

    command.Do();
    EXPECT_TRUE(std::filesystem::exists(m_Dir / "Stone.glsl"));
    EXPECT_TRUE(std::filesystem::exists(m_Dir / "Stone.material"));
    EXPECT_FALSE(std::filesystem::exists(m_Dir / "Rock.glsl"));
    EXPECT_FALSE(std::filesystem::exists(m_Dir / "Rock.material"));
    EXPECT_NE(ReadAll(m_Dir / "Stone.material").find("\"surfaceShader\": \"Stone.glsl\""), std::string::npos);
    EXPECT_NE(ReadAll(m_Dir / "Stone.material").find("\"materialName\": \"Stone\""), std::string::npos);
    EXPECT_EQ(ReadAll(m_Dir / "Stone.material").find("surfaceShaderGuid"), std::string::npos)
        << "the shader's old-path GUID must not survive the pair rename";
    ASSERT_EQ(renamedTo.size(), 1u);
    EXPECT_EQ(renamedTo.back(), m_Dir / "Stone.glsl");

    command.Undo();
    EXPECT_TRUE(std::filesystem::exists(m_Dir / "Rock.glsl"));
    EXPECT_TRUE(std::filesystem::exists(m_Dir / "Rock.material"));
    EXPECT_FALSE(std::filesystem::exists(m_Dir / "Stone.glsl"));
    EXPECT_NE(ReadAll(m_Dir / "Rock.material").find("\"surfaceShader\": \"Rock.glsl\""), std::string::npos);
    EXPECT_NE(ReadAll(m_Dir / "Rock.material").find("\"materialName\": \"Rock\""), std::string::npos);
    ASSERT_EQ(renamedTo.size(), 2u);
    EXPECT_EQ(renamedTo.back(), m_Dir / "Rock.glsl");

    command.Redo();
    EXPECT_TRUE(std::filesystem::exists(m_Dir / "Stone.glsl"));
    EXPECT_NE(ReadAll(m_Dir / "Stone.material").find("\"surfaceShader\": \"Stone.glsl\""), std::string::npos);
    EXPECT_EQ(renamedTo.size(), 3u);
}

// POSIX rename replaces an existing destination, so an undo into a name some
// other file has taken since the rename would silently destroy that file. The
// command refuses and both files survive; the same guard covers redo.
TEST_F(AssetRenameTests, UndoIntoAnOccupiedNameRefusesAndPreservesBothFiles)
{
    const std::filesystem::path mat = Write("Rock.material", "{\n  \"materialName\": \"Rock\"\n}\n");
    RenameAssetCommand command(PlanAssetRename(mat, "Stone"), /*assets=*/nullptr, {});
    command.Do();
    ASSERT_TRUE(std::filesystem::exists(m_Dir / "Stone.material"));

    // A file re-created at the old name (externally, or by Create > Material).
    Write("Rock.material", "{\n  \"materialName\": \"Occupant\"\n}\n");

    command.Undo();
    EXPECT_TRUE(std::filesystem::exists(m_Dir / "Stone.material"));
    EXPECT_NE(ReadAll(m_Dir / "Rock.material").find("\"materialName\": \"Occupant\""), std::string::npos)
        << "the occupying file must survive an undo into its name";
    EXPECT_NE(ReadAll(m_Dir / "Stone.material").find("\"materialName\": \"Stone\""), std::string::npos);
}

TEST_F(AssetRenameTests, RedoIntoAnOccupiedNameRefusesAndPreservesBothFiles)
{
    const std::filesystem::path mat = Write("Rock.material", "{\n  \"materialName\": \"Rock\"\n}\n");
    RenameAssetCommand command(PlanAssetRename(mat, "Stone"), /*assets=*/nullptr, {});
    command.Do();
    command.Undo();
    ASSERT_TRUE(std::filesystem::exists(m_Dir / "Rock.material"));

    Write("Stone.material", "{\n  \"materialName\": \"Occupant\"\n}\n");

    command.Redo();
    EXPECT_TRUE(std::filesystem::exists(m_Dir / "Rock.material"));
    EXPECT_NE(ReadAll(m_Dir / "Stone.material").find("\"materialName\": \"Occupant\""), std::string::npos)
        << "the occupying file must survive a redo into its name";
    EXPECT_NE(ReadAll(m_Dir / "Rock.material").find("\"materialName\": \"Rock\""), std::string::npos);
}

// The pair renames atomically: refusing only the companion after the primary
// succeeded would leave a renamed shader whose material still sits at the old
// stem with a broken surfaceShader reference. When only the COMPANION's
// destination is occupied, the whole rename is refused and every file stays put.
TEST_F(AssetRenameTests, PairRenameWithAnOccupiedCompanionDestinationRefusesWhole)
{
    const std::filesystem::path shader = WriteSurfacePair("Rock");
    // Stone.glsl is free; only the companion's destination is taken.
    Write("Stone.material", "{\n  \"materialName\": \"Occupant\"\n}\n");

    RenameAssetCommand command(PlanAssetRename(shader, "Stone"), /*assets=*/nullptr, {});
    command.Do();

    EXPECT_TRUE(std::filesystem::exists(m_Dir / "Rock.glsl")) << "the primary must not half-apply";
    EXPECT_FALSE(std::filesystem::exists(m_Dir / "Stone.glsl"));
    EXPECT_TRUE(std::filesystem::exists(m_Dir / "Rock.material"));
    EXPECT_NE(ReadAll(m_Dir / "Stone.material").find("\"materialName\": \"Occupant\""), std::string::npos)
        << "the occupying file must survive";
    EXPECT_NE(ReadAll(m_Dir / "Rock.material").find("\"surfaceShader\": \"Rock.glsl\""), std::string::npos)
        << "the pair must keep referencing each other";
}

TEST_F(AssetRenameTests, CommandOnAMaterialAloneLeavesOtherFilesUntouched)
{
    const std::filesystem::path shader = WriteSurfacePair("Rock");
    RenameAssetCommand command(PlanAssetRename(m_Dir / "Rock.material", "Stone"), nullptr, {});
    command.Do();
    EXPECT_TRUE(std::filesystem::exists(m_Dir / "Stone.material"));
    EXPECT_TRUE(std::filesystem::exists(shader));
    EXPECT_FALSE(std::filesystem::exists(m_Dir / "Stone.glsl"));
    // The material keeps pointing at the shader it had; only its own name follows.
    const std::string text = ReadAll(m_Dir / "Stone.material");
    EXPECT_NE(text.find("\"surfaceShader\": \"Rock.glsl\""), std::string::npos) << text;
    EXPECT_NE(text.find("\"materialName\": \"Stone\""), std::string::npos) << text;
}

TEST_F(AssetRenameRegistryTests, ResolvesARelativePlanPathToTheRegistryAbsoluteForm)
{
    const std::filesystem::path onDisk = WriteMaterial("NewSurface.material");
    RegisterProjectSource();

    GameEngine::AssetMetadata metadata;
    ASSERT_TRUE(m_Assets->GetRegistry().TryGetAssetMetadata(onDisk, metadata));
    ASSERT_TRUE(metadata.Path.is_absolute());

    EXPECT_EQ(ResolveRegistryAssetPath(m_Assets.get(), "NewSurface.material"), metadata.Path);
    EXPECT_EQ(ResolveRegistryAssetPath(m_Assets.get(), onDisk), metadata.Path);

    // Anything the registry cannot resolve passes through unchanged.
    EXPECT_EQ(ResolveRegistryAssetPath(m_Assets.get(), "NotRegistered.material"),
              std::filesystem::path("NotRegistered.material"));
    EXPECT_EQ(ResolveRegistryAssetPath(nullptr, "NewSurface.material"),
              std::filesystem::path("NewSurface.material"));
    EXPECT_TRUE(ResolveRegistryAssetPath(m_Assets.get(), {}).empty());
}

// The rename-purge defect shape: the compile lane keys shader-error entries by
// the registry's absolute metadata path, the rename plan holds the browser's
// project-relative path. The compare-time fold cannot re-base, so the raw plan
// path must NOT clear the entry (control arm) and the resolved form must.
TEST_F(AssetRenameRegistryTests, AbsoluteKeyedShaderErrorEntryIsPurgedViaTheResolvedPlanPath)
{
    const std::filesystem::path onDisk = WriteMaterial("NewSurface.material");
    RegisterProjectSource();

    GameEngine::AssetMetadata metadata;
    ASSERT_TRUE(m_Assets->GetRegistry().TryGetAssetMetadata(onDisk, metadata));

    GameEngine::Engine::Renderer::ShaderCompileErrorLog log;
    GameEngine::Engine::Renderer::ShaderCompileErrorLog::Entry entry;
    entry.MaterialName = "NewSurface";
    entry.SurfaceShaderPath = "NewSurface.glsl";
    entry.MaterialAssetPath = metadata.Path;
    entry.Errors = {"NewSurface.glsl:1: error: ';' expected"};
    log.ReportFailure(std::move(entry));

    // Control: the un-resolved relative plan path cannot match the absolute
    // key — the log has no project root to re-base against, by design.
    log.RemoveMaterialAsset("NewSurface.material");
    ASSERT_EQ(log.Count(), 1u);

    log.RemoveMaterialAsset(ResolveRegistryAssetPath(m_Assets.get(), "NewSurface.material"));
    EXPECT_EQ(log.Count(), 0u)
        << "the registry-resolved plan path must clear the absolute-keyed entry";
}

// A folder's whole name is what a rename edits: a dot in it is part of the name, not an
// extension to keep.
TEST_F(AssetRenameTests, AFolderRenameEditsTheWholeNameAndRefusesASiblingsName)
{
    const std::filesystem::path folder = m_Dir / "Props.old";
    std::filesystem::create_directories(folder);
    std::filesystem::create_directories(m_Dir / "Taken");

    EXPECT_EQ(PlanAssetRename(folder, "Props.v2").To, m_Dir / "Props.v2");

    const AssetRenameValidation clash = ValidateAssetRename("Taken", folder);
    EXPECT_FALSE(clash.Ok);
    EXPECT_EQ(clash.Reason, "'Taken' already exists here. Choose another name.");
    EXPECT_TRUE(ValidateAssetRename("Props.v2", folder).Ok);
}

TEST_F(AssetRenameTests, CommandRenamesAFolderWithItsContentsAndUndoRestoresIt)
{
    const std::filesystem::path folder = m_Dir / "Props";
    std::filesystem::create_directories(folder);
    {
        std::ofstream out(folder / "Rock.material", std::ios::binary);
        out << "{}\n";
    }

    RenameAssetCommand command(PlanAssetRename(folder, "Scenery"), nullptr, {});
    EXPECT_STREQ(command.GetName(), "Rename Folder");
    command.Do();
    EXPECT_FALSE(std::filesystem::exists(folder));
    EXPECT_TRUE(std::filesystem::exists(m_Dir / "Scenery" / "Rock.material"));

    command.Undo();
    EXPECT_TRUE(std::filesystem::exists(folder / "Rock.material"));
    EXPECT_FALSE(std::filesystem::exists(m_Dir / "Scenery"));
}

// The assets under a renamed folder keep their identity: each registration follows its file.
TEST_F(AssetRenameRegistryTests, AFolderRenameKeepsEachAssetsGuidAtItsNewPath)
{
    std::filesystem::create_directories(m_Dir / "Props");
    const std::filesystem::path onDisk = WriteMaterial("Props/NewSurface.material");
    RegisterProjectSource();
    const GameEngine::GUID guid = m_Assets->GetRegistry().GetAssetGUID(onDisk);
    ASSERT_FALSE(guid.IsNull());

    RenameAssetCommand command(PlanAssetRename(m_Dir / "Props", "Scenery"), m_Assets.get(), {});
    command.Do();

    GameEngine::AssetMetadata metadata;
    ASSERT_TRUE(m_Assets->GetRegistry().TryGetAssetMetadata(guid, metadata))
        << "the folder rename dropped the registration of an asset inside it";
    EXPECT_TRUE(std::filesystem::equivalent(metadata.Path, m_Dir / "Scenery" / "NewSurface.material"))
        << metadata.Path.string();

    command.Undo();
    ASSERT_TRUE(m_Assets->GetRegistry().TryGetAssetMetadata(guid, metadata));
    EXPECT_TRUE(std::filesystem::equivalent(metadata.Path, onDisk)) << metadata.Path.string();
}

// F2 renames a folder in the folder tree; the folder's context menu offers the same.
TEST(AssetFolderMenu, AFoldersMenuOffersRenameAboveDelete)
{
    GameEngine::ContextMenuBuilder builder;
    GameEngine::AddDirectoryEditItems(builder);
    EditorTesting::RecordingContextMenu menu;
    builder.Build(&menu);

    EXPECT_EQ(menu.CommandOf("Rename"), GameEngine::kDirectoryCmdRename) << "the folder's menu has no Rename";
    EXPECT_EQ(menu.CommandOf("Delete"), GameEngine::kDirectoryCmdDelete);
    ASSERT_EQ(menu.Rows.size(), 2u);
    EXPECT_EQ(menu.Rows.front().Title, "Rename");
}

namespace {

// The Assets views reuse pooled rows and cells on a scroll. The folder tree's title is a child of
// its row, the list's Name label sits in a cell of its row and the grid's title is a child of its
// cell. Two such rows; the first one shows the entry being renamed.
class AssetRenameRowBoundTests : public ::testing::Test
{
protected:
    void Build(bool titleInCell)
    {
        auto root = std::make_unique<GameEngine::UIElement>();
        root->SetFocusable(true);
        m_View = root.get();
        GameEngine::Label* unused = nullptr;
        m_Row = AddRow(*root, titleInCell, m_Title);
        m_OtherRow = AddRow(*root, titleInCell, unused);
        if (!m_Fixture.Build(std::move(root)))
            GTEST_SKIP() << m_Fixture.Diagnostic;
    }

    // Opens the rename of `renamed` in the first row, binds the rows as a scroll does, and
    // expects the field to stay while its row shows `renamed` and to end once it shows `other`.
    void ExpectTheFieldEndsWhenItsRowShowsAnotherEntry(const std::filesystem::path& renamed,
                                                       const std::filesystem::path& other, const char* fieldClass)
    {
        m_Rename.Begin(m_Title, m_View, renamed, fieldClass, nullptr, nullptr, {});
        Frame();
        ASSERT_NE(Field(), nullptr) << "the rename did not open; the test is vacuous";

        m_Rename.OnRowBound(*m_OtherRow, other);
        m_Rename.OnRowBound(*m_Row, renamed);
        Frame();
        EXPECT_NE(Field(), nullptr) << "binding the row to the entry it renames closed the field";

        m_Rename.OnRowBound(*m_Row, other);
        Frame();
        EXPECT_EQ(Field(), nullptr) << "the field stayed on a row that shows another entry";
        const auto display = m_Title->Overrides().Get(GameEngine::Style::Display);
        EXPECT_FALSE(display.has_value() && *display == GameEngine::DisplayMode::None) << "the title did not come back";
    }

private:
    static GameEngine::UIElement* AddRow(GameEngine::UIElement& view, bool titleInCell, GameEngine::Label*& title)
    {
        auto row = std::make_unique<GameEngine::UIElement>();
        GameEngine::UIElement* rowRaw = row.get();
        GameEngine::UIElement* host = rowRaw;
        if (titleInCell)
        {
            auto cell = std::make_unique<GameEngine::UIElement>();
            host = cell.get();
            row->AddChild(std::move(cell));
        }
        auto label = std::make_unique<GameEngine::Label>();
        label->SetText("Rock");
        title = label.get();
        host->AddChild(std::move(label));
        view.AddChild(std::move(row));
        return rowRaw;
    }

    GameEngine::UIElement* Field() { return InspectorLayoutTesting::FindByClass(m_Row, "inline-rename-field"); }
    void Frame() { m_Fixture.Ui->Update(1.0f / 60.0f, /*interactive=*/true); }

    InspectorLayoutTesting::InspectorLayoutFixture m_Fixture;
    GameEngine::AssetRenameController m_Rename;
    GameEngine::UIElement* m_View = nullptr;
    GameEngine::UIElement* m_Row = nullptr;
    GameEngine::UIElement* m_OtherRow = nullptr;
    GameEngine::Label* m_Title = nullptr;
};

} // namespace

TEST_F(AssetRenameRowBoundTests, AFolderTreeRenameEndsWhenItsRowShowsAnotherFolder)
{
    Build(/*titleInCell=*/false);
    if (IsSkipped())
        return;
    ExpectTheFieldEndsWhenItsRowShowsAnotherEntry("Assets/Props", "Assets/Rocks", "tree-rename-field");
}

TEST_F(AssetRenameRowBoundTests, AListRenameEndsWhenItsRowShowsAnotherAsset)
{
    Build(/*titleInCell=*/true);
    if (IsSkipped())
        return;
    ExpectTheFieldEndsWhenItsRowShowsAnotherEntry("Assets/Rock.png", "Assets/Moss.png", nullptr);
}

TEST_F(AssetRenameRowBoundTests, AGridRenameEndsWhenItsCellShowsAnotherAsset)
{
    Build(/*titleInCell=*/false);
    if (IsSkipped())
        return;
    ExpectTheFieldEndsWhenItsRowShowsAnotherEntry("Assets/Rock.png", "Assets/Moss.png", "grid-rename-field");
}
