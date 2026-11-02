// The material system's compile tasks report to the scene load tracker: each counts once,
// in its own category, under the name of its material. Every worker of the engine's pool is
// held while a task is submitted, so the task stays queued until the test releases them.

#include "EngineLogCapture.h"
#include "HeldAssetImportFixture.h"

#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Engine/Rendering/PipelineVariantCache.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "SceneLoadTracker/SceneLoadTracker.h"
#include "SceneLoadTrackerTestAccess.h"
#include "StagedTestPaths.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Rendering;

namespace
{

// Occupies every worker of the engine's pool until Release, so a task submitted meanwhile stays queued.
class WorkerHold
{
  public:
    WorkerHold()
    {
        JobSystem::WorkStealingThreadPool& pool = EngineCore::GetInstance().GetJobSystem();
        m_Workers = static_cast<int>(pool.GetWorkerCount());
        for (int worker = 0; worker < m_Workers; ++worker)
            pool.EnqueueWork([gate = m_Gate] { gate->Hold(); });
    }
    ~WorkerHold() { Release(); }
    WorkerHold(const WorkerHold&) = delete;
    WorkerHold& operator=(const WorkerHold&) = delete;

    bool AllHeld() const { return m_Gate->WaitForEntered(m_Workers); }
    void Release() { m_Gate->Release(); }

  private:
    std::shared_ptr<Testing::ImportGate> m_Gate = std::make_shared<Testing::ImportGate>();
    int m_Workers = 0;
};

// Releases `hold` when a held compile task is destroyed unrun (its count of completed tasks moves
// past `completedBefore`), or after 10 s. Runs on a thread of its own: every pool worker is held.
void ReleaseAfterTheFirstCancel(const MaterialSystem& materials, WorkerHold& hold, uint64_t completedBefore)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (materials.GetPrewarmProgress().completed == completedBefore && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    hold.Release();
}

MaterialDocument StandardDocument(const char* name)
{
    MaterialDocument doc{};
    doc.materialName = name;
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";
    return doc;
}

class CompileLoadReportTest : public Testing::HeldAssetImportFixture
{
  protected:
    void SetUp() override
    {
        HeldAssetImportFixture::SetUp();
        if (IsSkipped() || HasFatalFailure())
            return;
        m_Renderer = std::make_unique<RenderServices>();
        ASSERT_TRUE(m_Renderer->Initialize(m_Device.get()));
        // The staged shaders, so a compile reads real GLSL.
        const std::filesystem::path shaders = TestPaths::StagedRenderingShadersDir();
        ASSERT_TRUE(std::filesystem::exists(shaders)) << shaders.string();
        MaterialBuildContext context{};
        context.AdapterShaderDir = shaders;
        context.CacheRoot = std::filesystem::temp_directory_path() / "ge_test_shader_cache";
        context.IncludeDirs = {shaders};
        Materials().SetMaterialBuildContext(context);
    }

    // A material system that submitted a task drains the engine's pool when it is destroyed,
    // so it goes before the engine shuts down.
    void TearDown() override
    {
        if (m_Renderer)
            m_Renderer->Shutdown();
        m_Renderer.reset();
        HeldAssetImportFixture::TearDown();
    }

    MaterialSystem& Materials() { return m_Renderer->Materials(); }

    // Ticks the tracker and returns the row of `id`.
    const SceneLoadTracker::CategoryStatus& Row(const char* id)
    {
        SceneLoadTrackerTestAccess::Tick(m_Tracker,std::chrono::steady_clock::now());
        for (const SceneLoadTracker::CategoryStatus& row : m_Tracker.GetStatus().Categories)
            if (row.Id == id)
                return row;
        ADD_FAILURE() << "the material system reports no " << id << " row";
        static const SceneLoadTracker::CategoryStatus kNone{};
        return kNone;
    }

    // Ticks until the row of `id` has `done` tickets done and none open, for at most 10 s.
    bool WaitUntilDone(const char* id, uint64_t done)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline)
        {
            const SceneLoadTracker::CategoryStatus& row = Row(id);
            if (row.Done >= done && row.Queued == 0 && row.Running == 0 && row.Background == 0)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }

    SceneLoadTracker m_Tracker;
    std::unique_ptr<RenderServices> m_Renderer;
};

} // namespace

// A base compile that a draw waits for holds the query, named by its material.
TEST_F(CompileLoadReportTest, AnOnDemandBaseCompileHoldsTheQueryUnderItsMaterialsName)
{
    Materials().ReportLoadingWork(m_Tracker);
    WorkerHold hold;
    ASSERT_TRUE(hold.AllHeld());
    Materials().PrewarmMaterialBaseShader(StandardDocument("HeldBaseCompile"), {});

    const SceneLoadTracker::CategoryStatus& queued = Row("shader.base-compile");
    EXPECT_EQ(queued.Queued, 1u);
    EXPECT_EQ(m_Tracker.NameOf(queued.OldestPending), "HeldBaseCompile");
    EXPECT_FALSE(m_Tracker.IsReadyToShow());

    hold.Release();
    EXPECT_TRUE(WaitUntilDone("shader.base-compile", 1)) << m_Tracker.Describe();
    EXPECT_TRUE(m_Tracker.IsReadyToShow()) << m_Tracker.Describe();
}

// The project warm-up compiles for later scenes: it is counted and never holds the query.
TEST_F(CompileLoadReportTest, TheProjectWarmUpIsCountedAndNeverHolds)
{
    Materials().ReportLoadingWork(m_Tracker);
    WorkerHold hold;
    ASSERT_TRUE(hold.AllHeld());
    Materials().PrewarmMaterialShaders(StandardDocument("HeldWarmUp"), {}, MaterialKeyword::None);

    const SceneLoadTracker::CategoryStatus& warming = Row("shader.base-compile");
    EXPECT_EQ(warming.Background, 1u);
    EXPECT_FALSE(warming.HoldsReady());
    EXPECT_TRUE(m_Tracker.IsReadyToShow()) << m_Tracker.Describe();

    hold.Release();
    EXPECT_TRUE(WaitUntilDone("shader.base-compile", 1)) << m_Tracker.Describe();
}

// A draw's variant compile counts once, in shader.variant, named by its material: a ticket from
// its submit to the end of the compile, then the gate until the variant is published.
TEST_F(CompileLoadReportTest, ADrawVariantCompileCountsOnceInShaderVariantUnderItsMaterialsName)
{
    Material* material = Materials().RegisterMaterialFromDocument(GUID::Generate(), StandardDocument("HeldVariant"));
    ASSERT_NE(material, nullptr);
    Materials().FlushAsyncMaterialCompiles();
    ASSERT_TRUE(material->GetGraphicsPipelineId().IsValid());
    Materials().ReportLoadingWork(m_Tracker);

    PipelineVariantCache& variants = Materials().Variants();
    PipelineFormatKey formats{};
    formats.ColorCount = 1;
    formats.ColorFormats[0] = TextureFormat::R16G16B16A16_FLOAT;
    formats.DepthFormat = TextureFormat::D32_FLOAT;
    const MaterialKeyword passKeywords =
        MaterialKeyword::ForwardPlus | MaterialKeyword::Instanced | MaterialKeyword::Shadows;
    WorkerHold hold;
    ASSERT_TRUE(hold.AllHeld());
    variants.WarmColorVariant(*material, VertexAttributeFlags::StandardMesh | VertexAttributeFlags::HasColor,
                              PrimitiveTopology::TriangleList, passKeywords, FrontFace::CounterClockwise, formats);
    variants.SubmitPendingVariantCompiles();

    const SceneLoadTracker::CategoryStatus& variant = Row("shader.variant");
    EXPECT_EQ(variant.Queued, 1u);
    EXPECT_EQ(m_Tracker.NameOf(variant.OldestPending), "HeldVariant");
    EXPECT_EQ(variant.WaitingGates, 0u) << "the submitted compile also counts in the gate";
    const SceneLoadTracker::CategoryStatus& base = Row("shader.base-compile");
    EXPECT_EQ(base.Queued + base.Running, 0u) << "the variant compile also counts as a base compile";

    hold.Release();
    EXPECT_TRUE(WaitUntilDone("shader.variant", 1)) << m_Tracker.Describe();
    EXPECT_EQ(Row("shader.variant").WaitingGates, 1u) << "the compiled variant is not published yet";
    variants.ApplyPendingVariantPublishes();
    EXPECT_FALSE(Row("shader.variant").HoldsReady()) << m_Tracker.Describe();
}

// A quit during a cold start cancels the queued compiles on purpose: no compile is logged as
// dropped or counted as failed. The base compiles fill the compile gate, so the last of them
// and the draw variant compile wait in the gate until the shutdown cancels them.
TEST_F(CompileLoadReportTest, AQuitCancelsTheQueuedCompilesWithoutFailingThem)
{
    Material* material = Materials().RegisterMaterialFromDocument(GUID::Generate(), StandardDocument("QuitVariant"));
    ASSERT_NE(material, nullptr);
    Materials().FlushAsyncMaterialCompiles();
    ASSERT_TRUE(material->GetGraphicsPipelineId().IsValid());
    Materials().ReportLoadingWork(m_Tracker);

    std::vector<std::string> lines;
    TestLog::ScopedEngineLogCapture capture(&lines);
    Logger::Log::Info("CompileLoadReportTest: the capture is live");

    MaterialSystem& materials = Materials();
    WorkerHold hold;
    ASSERT_TRUE(hold.AllHeld());
    const uint64_t completedBefore = materials.GetPrewarmProgress().completed;
    // The gate admits fewer compiles than the pool has workers.
    const std::size_t compiles = EngineCore::GetInstance().GetJobSystem().GetWorkerCount() + 1;
    for (std::size_t index = 0; index < compiles; ++index)
        materials.PrewarmMaterialBaseShader(StandardDocument("QuitBaseCompile"), {});
    PipelineFormatKey formats{};
    formats.ColorCount = 1;
    formats.ColorFormats[0] = TextureFormat::R16G16B16A16_FLOAT;
    formats.DepthFormat = TextureFormat::D32_FLOAT;
    materials.Variants().WarmColorVariant(
        *material, VertexAttributeFlags::StandardMesh | VertexAttributeFlags::HasColor, PrimitiveTopology::TriangleList,
        MaterialKeyword::ForwardPlus | MaterialKeyword::Instanced | MaterialKeyword::Shadows,
        FrontFace::CounterClockwise, formats);
    materials.Variants().SubmitPendingVariantCompiles();
    EXPECT_EQ(Row("shader.base-compile").Queued, compiles);
    EXPECT_EQ(Row("shader.variant").Queued, 1u);

    // The shutdown cancels the queued compiles and then waits for the admitted ones, which run
    // only on released workers: release them when the first canceled compile is destroyed.
    std::thread releaser(ReleaseAfterTheFirstCancel, std::cref(materials), std::ref(hold), completedBefore);
    m_Renderer->Shutdown();
    releaser.join();
    Logger::Log::Flush();

    EXPECT_EQ(TestLog::CountLinesContaining(lines, "the capture is live"), 1u);
    const std::string canceled = TestLog::FirstLineContaining(lines, "shutdown canceled ");
    ASSERT_FALSE(canceled.empty());
    EXPECT_GE(std::stoul(canceled.substr(canceled.find("canceled ") + 9)), 2u) << canceled;
    EXPECT_EQ(TestLog::CountLinesContaining(lines, "dropped before it completed"), 0u)
        << TestLog::FirstLineContaining(lines, "dropped before it completed");
    SceneLoadTrackerTestAccess::Tick(m_Tracker,std::chrono::steady_clock::now());
    for (const SceneLoadTracker::CategoryStatus& row : m_Tracker.GetStatus().Categories)
        EXPECT_EQ(row.Failed, 0u) << row.Id;
    m_Renderer.reset();
}
