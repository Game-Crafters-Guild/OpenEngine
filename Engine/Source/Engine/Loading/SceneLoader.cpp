#include "Engine/Loading/SceneLoader.h"

#include "Assets/AssetManager.h"
#include "Components/Rendering/Camera.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECSModules/Rendering/RenderingLoop.h"
#include "Engine/Loading/SceneLoadInstantiated.h"
#include "Engine/Rendering/SceneResolveService.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"
#include "NativeScripting/UserSystemRegistry.h"
#include "SceneLoadState.h"
#include "SplineRecipes/SplineRecipeSystem.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

namespace GameEngine
{
namespace
{
using Clock = std::chrono::steady_clock;
using Microseconds = std::chrono::microseconds;
using Renderer = Engine::Renderer::RenderingLoop;

double SecondsSince(Clock::time_point start)
{
    return std::chrono::duration<double>(Clock::now() - start).count();
}

std::string DescribeError(const Scene::SceneIOError& error)
{
    std::string text = error.file.generic_string();
    if (error.line > 0)
        text += ":" + std::to_string(error.line);
    if (!text.empty())
        text += ": ";
    return text + error.message;
}

// Runs on a job worker: the read uses no world, and the resolver's registry calls lock. An
// "alias:path" reads the AssetManager's source list, which has no lock: mount no source during a read.
// The loader keeps the state until it sees ReadDone, so the job holds no reference to it.
void ReadOnWorker(SceneLoadState& state)
{
    state.ReadTicket.Start();
    state.Read = Scene::ReadSceneDocument(state.SceneFile, state.ReadOptions);
    if (state.Read)
        state.ReadTicket.Complete();
    else
        state.ReadTicket = {}; // WorkTicket has no Fail: an abandoned ticket counts as failed
    state.ReadDone.store(true, std::memory_order_release);
}

// Main thread. After ReadDone the read job does not write the document, so this empties it: a held
// operation of a cancelled load keeps no parsed scene. Returns whether the read has ended.
bool ReleaseDocumentIfReadEnded(SceneLoadState& state)
{
    if (!state.ReadDone.load(std::memory_order_acquire))
        return false;
    state.Read.Document = {};
    return true;
}

// The outgoing scene's generated spline content is released against the world it was
// generated in, before the clear (SplineRecipeSystem).
void ReleaseSceneScopedContent(ECS::World& world)
{
    auto* loop = EngineCore::GetInstance().GetRenderingLoop();
    auto* systems = loop ? loop->GetSystemManager() : nullptr;
    if (auto* recipes = systems ? systems->GetSystem<SplineRecipes::SplineRecipeSystem>() : nullptr)
        recipes->ReleaseSceneScoped(world);
}

bool IsSingle(const std::shared_ptr<SceneLoadState>& state)
{
    return state->Request.Mode == SceneLoadMode::Single;
}

constexpr std::string_view kAnotherLoadInProgress =
    "another scene load is in progress: start the additive load after that load raises Revealed, Failed or Cancelled";

// Fails an Additive load that never started because another load was in progress. It began no
// ticket, so the tracker counts nothing for it.
void Refuse(SceneLoadState& state)
{
    state.Current = SceneLoadState::Step::Failed;
    Logger::Log::Error("SceneLoad: cannot load '{}': {}", state.Request.Scene.generic_string(), kAnotherLoadInProgress);
    state.Failed.Invoke(kAnotherLoadInProgress);
}
} // namespace

SceneLoader::SceneLoader(SceneLoadTracker& tracker)
    : m_Tracker(tracker),
      m_ReadCategory(tracker.RegisterCategory(
          {.Id = "scene.read", .Label = "Reading the scene", .Unit = SceneLoadTracker::CostUnit::Files})),
      m_InstantiateCategory(tracker.RegisterCategory(
          {.Id = "scene.instantiate", .Label = "Building the scene", .Unit = SceneLoadTracker::CostUnit::Files}))
{
}

SceneLoader::~SceneLoader()
{
    Shutdown();
}

SceneLoadOperation SceneLoader::Load(SceneLoadRequest request)
{
    auto state = std::make_shared<SceneLoadState>();
    state->Request = std::move(request);
    state->Started = Clock::now();
    state->Progress = SceneLoadProgress(state->Started);
    m_Queued.push_back(state);
    return SceneLoadOperation(std::move(state));
}

bool SceneLoader::IsLoading() const
{
    return m_Current != nullptr || !m_Queued.empty();
}

void SceneLoader::Tick(Clock::time_point now, Microseconds gpuWait)
{
    const Clock::time_point tickStart = Clock::now();
    // A cancelled load is released here, on the main thread, once its read has ended.
    std::erase_if(m_Draining,
                  [](const std::shared_ptr<SceneLoadState>& state) { return ReleaseDocumentIfReadEnded(*state); });
    if (!m_Queued.empty())
        StartQueued();
    if (m_Current)
        Advance();
    if (m_Current)
        UpdateProgress(now);
    if (m_Current && m_Current->ReadyToShow)
        UpdateReveal();
    UpdateBudget(now, std::chrono::duration_cast<Microseconds>(Clock::now() - tickStart), gpuWait);
}

void SceneLoader::Shutdown()
{
    m_Queued.clear();
    if (m_Current && IsReadInFlight(*m_Current))
        m_Draining.push_back(m_Current);
    m_Current.reset();
    m_PausedCameras.clear();
    for (const std::shared_ptr<SceneLoadState>& state : m_Draining)
    {
        state->ReadTask.Wait();
        state->Read.Document = {}; // the read has ended: a held operation keeps no parsed scene
    }
    m_Draining.clear();
}

bool SceneLoader::IsReadInFlight(const SceneLoadState& state)
{
    return state.Current == SceneLoadState::Step::Reading && !state.ReadDone.load(std::memory_order_acquire);
}

void SceneLoader::StartQueued()
{
    std::vector<std::shared_ptr<SceneLoadState>> queued = std::move(m_Queued);
    m_Queued.clear();
    std::shared_ptr<SceneLoadState> next;
    std::vector<std::shared_ptr<SceneLoadState>> replaced;
    std::vector<std::shared_ptr<SceneLoadState>> refused;
    for (std::shared_ptr<SceneLoadState>& state : queued)
    {
        if (IsSingle(state))
        {
            if (next)
                replaced.push_back(std::move(next));
            next = std::move(state);
        }
        else if (next || m_Current)
            refused.push_back(std::move(state));
        else
            next = std::move(state);
    }
    // The load that starts is current before the others raise Cancelled or Failed, so IsLoading()
    // is true in their handlers. An Additive load starts only when no load is current.
    if (next)
    {
        const std::shared_ptr<SceneLoadState> previous = std::exchange(m_Current, nullptr);
        Begin(next);
        if (previous)
            Cancel(previous);
    }
    for (const std::shared_ptr<SceneLoadState>& older : replaced)
        Cancel(older);
    for (const std::shared_ptr<SceneLoadState>& state : refused)
        Refuse(*state);
}

void SceneLoader::Begin(const std::shared_ptr<SceneLoadState>& state)
{
    auto& engine = EngineCore::GetInstance();
    const std::filesystem::path& assetRoot = engine.GetResolvedAssetRoot();
    state->SceneFile = assetRoot / state->Request.Scene;
    state->Resolver = std::make_unique<Scene::AssetManagerSceneResolver>(
        engine.GetAssetManager(), assetRoot, Scene::AssetManagerSceneResolver::UnknownFile::AssignGuid);
    state->ReadOptions = {.AssetResolver = state->Resolver.get(),
                          .AssetRootOverride = assetRoot,
                          .ProjectRoot = engine.GetWorkspaceRoot()};
    state->ReadTicket =
        m_ReadCategory.BeginWork(m_Tracker.Intern(state->SceneFile.generic_string()), 1.0);
    state->Current = SceneLoadState::Step::WaitingForScan;
    m_Current = state;
    // A load with the switch takes over the pauses of the load that it replaces, so no frame draws the
    // world between the two. A load without the switch resumes them.
    if (state->Request.PauseWorldDrawing)
        PauseWorldDrawing();
    else
        ResumeWorldDrawing();
    Logger::Log::Info("SceneLoad: loading '{}'", state->Request.Scene.generic_string());
}

void SceneLoader::Advance()
{
    SceneLoadState& state = *m_Current;
    switch (state.Current)
    {
        case SceneLoadState::Step::WaitingForScan:
            // The read looks up asset GUIDs, so it starts once the project's startup scan
            // has registered every file. The main thread never waits for the scan.
            if (EngineCore::GetInstance().GetAssetManager().GetRegistry().IsStartupScanRunning(
                    kAssetSourceAliasProject))
                return;
            StartRead(m_Current);
            return;
        case SceneLoadState::Step::Reading:
            if (!state.ReadDone.load(std::memory_order_acquire))
                return;
            if (!state.Read)
            {
                Fail(DescribeError(*state.Read.Error));
                return;
            }
            Instantiate();
            return;
        case SceneLoadState::Step::WaitingForReady:
            // The tracker's snapshot of this frame was taken after the frame that
            // instantiated, so it holds the resolve work that the instantiate began.
            if (!m_Tracker.IsReadyToShow())
                return;
            if (!state.Request.PauseWorldDrawing)
            {
                state.ReadyToShow = true;
                return;
            }
            // This snapshot covers a paused frame. The count starts with the snapshot of the next frame.
            ResumeWorldDrawing();
            Logger::Log::Info("SceneLoad: '{}' resumed world drawing {:.2f} s after Load",
                              state.Request.Scene.generic_string(), SecondsSince(state.Started));
            state.Current = SceneLoadState::Step::WaitingForReadyFramesAfterResume;
            return;
        case SceneLoadState::Step::WaitingForReadyFramesAfterResume:
            CountReadyFramesAfterResume();
            return;
        default:
            return;
    }
}

void SceneLoader::StartRead(const std::shared_ptr<SceneLoadState>& state)
{
    state->Current = SceneLoadState::Step::Reading;
    state->ReadTask = EngineCore::GetInstance().GetJobSystem().Submit([raw = state.get()] { ReadOnWorker(*raw); });
    if (!state->ReadTask.IsValid())
    {
        // The job system refused the job, so the read never runs.
        state->ReadTicket = {};
        state->ReadDone.store(true, std::memory_order_release);
        Fail("the job system did not accept the scene read");
    }
}

void SceneLoader::Instantiate()
{
    SceneLoadState& state = *m_Current;
    auto& engine = EngineCore::GetInstance();
    ECS::World* world = engine.EnsurePrimaryWorld();
    if (!world)
    {
        Fail("the engine has no primary world");
        return;
    }

    SceneLoadTracker::WorkTicket ticket =
        m_InstantiateCategory.BeginWork(m_Tracker.Intern(state.SceneFile.generic_string()), 1.0);
    ticket.Start();
    // An Additive load keeps the world, its tick and the generated content of the scene it shows.
    const bool additive = state.Request.Mode == SceneLoadMode::Additive;
    if (!additive)
        ReleaseSceneScopedContent(*world);
    const Scene::SceneInstantiateResult result =
        Scene::InstantiateSceneDocument(*world, state.Read.Document, {.ClearWorldFirst = !additive});
    // The world keeps no reference into the document. Release it, so that a held operation does
    // not keep the parsed scene. Read.Timings stay for the log.
    state.Read.Document = {};
    if (!result)
    {
        ticket = {}; // WorkTicket has no Fail: an abandoned ticket counts as failed
        Fail(DescribeError(*result.Error));
        return;
    }
    // The clear destroyed the recorded cameras. OnInstantiated sees the new scene's cameras paused.
    if (!additive)
        m_PausedCameras.clear();
    if (state.Request.PauseWorldDrawing)
        PauseWorldDrawing();

    // Each entity binds on the frame its model has landed; the scene.resolve gate holds
    // the reveal until the batch is bound. The batch takes every entity of the world that waits
    // for its model, so an Additive load also waits for the running world's held entities.
    auto* loop = engine.GetRenderingLoop();
    if (auto* renderServices = engine.GetRenderServices(); loop && renderServices)
        (void)loop->GetSceneResolveService().EnqueueWorld(*world, *renderServices);

    SceneLoadInstantiated instantiated(*world, result.Degradation);
    if (state.Request.OnInstantiated)
        state.Request.OnInstantiated(instantiated);
    if (instantiated.m_FailureReason)
    {
        ticket = {};
        Fail(*instantiated.m_FailureReason);
        return;
    }
    ticket.Complete();

    // A later load keeps the user systems that run: OnStart runs once per session.
    if (!NativeScripting::AreUserSystemsStarted(*world))
        NativeScripting::StartUserSystems(*world);
    // A camera that OnInstantiated or a user system's OnStart made, such as the Player's default camera,
    // pauses with the others.
    if (state.Request.PauseWorldDrawing)
        PauseWorldDrawing();

    state.Current = SceneLoadState::Step::WaitingForReady;
    Logger::Log::Info("SceneLoad: '{}' instantiated, the world holds {} entities (read {:.1f} ms, clear {:.1f} ms, "
                      "instantiate {:.1f} ms)",
                      state.Request.Scene.generic_string(), world->GetEntityCount(),
                      state.Read.Timings.ReadMs + state.Read.Timings.ParseMs + state.Read.Timings.ValidateMs +
                          state.Read.Timings.SourcesMs,
                      result.Timings.ClearMs, result.Timings.InstantiateMs);
}

void SceneLoader::PauseWorldDrawing()
{
    ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld();
    if (!world)
        return;
    // A camera that is paused already is not recorded, so the loader does not resume it.
    world->Query<ECS::Write<Components::Camera>>().Each(
        [this](ECS::EntityHandle entity, Components::Camera& camera)
        {
            if (camera.WorldDrawingPaused)
                return;
            camera.WorldDrawingPaused = true;
            m_PausedCameras.push_back(entity);
        });
}

void SceneLoader::ResumeWorldDrawing()
{
    ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld();
    for (const ECS::EntityHandle entity : m_PausedCameras)
        if (auto* camera = world ? world->GetComponentForWrite<Components::Camera>(entity) : nullptr)
            camera->WorldDrawingPaused = false;
    m_PausedCameras.clear();
}

void SceneLoader::CountReadyFramesAfterResume()
{
    SceneLoadState& state = *m_Current;
    state.ReadyFramesInARow = m_Tracker.IsReadyToShow() ? state.ReadyFramesInARow + 1 : 0;
    if (state.ReadyFramesInARow >= kReadyFramesAfterResume)
        state.ReadyToShow = true;
}

void SceneLoader::UpdateProgress(Clock::time_point now)
{
    SceneLoadState& state = *m_Current;
    // From the resume, the content is ready, and the work that the live frames ask for has no estimate:
    // the estimate stays 0, and the bar moves to its last value before ready at the speed limit.
    const bool contentReady =
        state.ReadyToShow || state.Current == SceneLoadState::Step::WaitingForReadyFramesAfterResume;
    state.SecondsRemaining = contentReady ? 0.0 : m_Tracker.GetStatus().SecondsRemaining;
    state.Progress.Update(now, state.SecondsRemaining, state.ReadyToShow);
}

void SceneLoader::UpdateReveal()
{
    SceneLoadState& state = *m_Current;
    if (state.Current != SceneLoadState::Step::WaitingForReveal)
    {
        // A Ready handler can call Reveal(): the check below sees it in this frame.
        state.Current = SceneLoadState::Step::WaitingForReveal;
        state.Ready.Invoke();
    }
    if (state.Request.Reveal == SceneRevealPolicy::AtReady || state.RevealRequested)
        Reveal();
}

void SceneLoader::Reveal()
{
    const std::shared_ptr<SceneLoadState> state = std::exchange(m_Current, nullptr);
    state->Current = SceneLoadState::Step::Revealed;
    Logger::Log::Info("SceneLoad: '{}' revealed {:.2f} s after Load", state->Request.Scene.generic_string(),
                      SecondsSince(state->Started));
    state->Revealed.Invoke();
}

void SceneLoader::Fail(std::string_view reason)
{
    const std::shared_ptr<SceneLoadState> state = std::exchange(m_Current, nullptr);
    state->Current = SceneLoadState::Step::Failed;
    // The world draws in the Failed handlers, so a game can pause it again behind its error screen.
    ResumeWorldDrawing();
    const std::string text(reason);
    Logger::Log::Error("SceneLoad: cannot load '{}': {}", state->Request.Scene.generic_string(), text);
    state->Failed.Invoke(text);
}

void SceneLoader::Cancel(const std::shared_ptr<SceneLoadState>& state)
{
    // A submitted read, queued or running, ends on its worker, and the loader keeps the state
    // until then. Otherwise no read job writes the state, and the load gives up its document and its
    // read ticket now. The tracker counts the ticket of a load that waits for the scan as failed.
    if (IsReadInFlight(*state))
        m_Draining.push_back(state);
    else
    {
        state->ReadTicket = {};
        state->Read.Document = {};
    }
    state->Current = SceneLoadState::Step::Cancelled;
    Logger::Log::Info("SceneLoad: '{}' cancelled by a newer load", state->Request.Scene.generic_string());
    state->Cancelled.Invoke();
}

bool SceneLoader::IsReplacingScene() const
{
    return (m_Current && IsSingle(m_Current)) || std::ranges::any_of(m_Queued, IsSingle);
}

void SceneLoader::UpdateBudget(Clock::time_point now, Microseconds tickWork, Microseconds gpuWait)
{
    // While a Single load runs, the resolve slice takes what the frame bound leaves after the frame's
    // own main-thread work and this tick's load work. The frame's own work is the last frame's
    // period minus its load work and minus its GPU wait: the slice can use that wait.
    auto* loop = EngineCore::GetInstance().GetRenderingLoop();
    if (!loop)
        return;
    if (!IsReplacingScene())
    {
        if (m_BudgetActive)
            loop->SetResolveSlice(Renderer::kResolveFrameSlice);
        m_BudgetActive = false;
        return;
    }
    if (m_BudgetActive)
    {
        const Microseconds period = std::chrono::duration_cast<Microseconds>(now - m_LastTick);
        const Microseconds loadWork = m_LastTickWork + loop->LastResolveStepTime();
        const Microseconds frameCost = std::max(Microseconds{0}, period - loadWork - gpuWait);
        loop->SetResolveSlice(
            std::clamp(kFrameBound - frameCost - tickWork, Renderer::kResolveFrameSlice, kFrameBound));
    }
    m_BudgetActive = true;
    m_LastTick = now;
    m_LastTickWork = tickWork;
}
} // namespace GameEngine
