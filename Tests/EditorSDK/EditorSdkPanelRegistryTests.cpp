// EditorSDK phase 2 panel-registry semantics — the contracts panel-bearing
// Editor-kind package modules rely on:
//   * RegisterPanel REPLACES a same-PanelId registration (replace-forward
//     module hot-reload; nothing is ever unregistered),
//   * descriptors round-trip intact (the editor consumes title/icon and the
//     alias-scoped layout/style asset paths verbatim),
//   * consumers attached AFTER registrations get them replayed; later
//     registrations reach them at the next DeliverPendingRegistrations, never
//     inside the registering call (a module registers under the loader lock),
//   * editor-chrome stylesheet contributions dedupe by (alias, path) but
//     still re-notify (the downstream global attach is idempotent),
//   * OpenPanel dispatches through the editor-installed opener and is a loud
//     no-op without one.
//
// Links EditorSDK.dll — the same registry Editor.exe and module DLLs share.
// The registry is process-wide: every test uses its own ids and detaches its
// consumers before finishing.

#include "Editor/Registries/EditorPanelRegistry.h"
#include "UI/Controls/DockPanel.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ed = GameEngine::Editor;

namespace
{

ed::EditorPanelDescriptor MakeDescriptor(const char* panelId, const char* title)
{
    ed::EditorPanelDescriptor descriptor;
    descriptor.PanelId = panelId;
    descriptor.Title = title;
    descriptor.TabIconClass = "sdk-test-tab-icon";
    descriptor.AssetSourceAlias = "sdk-test-pack";
    descriptor.LayoutAssetPath = "UI/panels/SdkTest.uxml";
    descriptor.StyleAssetPath = "UI/panels/SdkTest.css";
    descriptor.DefaultDockAnchorPanelId = "SceneView";
    descriptor.Factory = [title]() -> std::unique_ptr<GameEngine::UIElement> {
        return std::make_unique<GameEngine::DockPanel>(title);
    };
    return descriptor;
}

std::size_t CountPanelsWithId(const std::vector<ed::EditorPanelDescriptor>& panels,
                              std::string_view panelId)
{
    return static_cast<std::size_t>(
        std::count_if(panels.begin(), panels.end(),
                      [&](const ed::EditorPanelDescriptor& d) { return d.PanelId == panelId; }));
}

struct ConsumerDetachGuard
{
    ~ConsumerDetachGuard() { ed::EditorPanelRegistry::Get().SetConsumers({}); }
};

// Stands in for the OS loader lock a module's static initializers run under: a
// thread can end only by taking it (LdrShutdownThread).
struct LoaderLockModel
{
    std::mutex LoaderLock;
    bool DestructorTimedOut = false;
    std::vector<std::thread> Unjoined;
};

// A panel whose destructor waits for the worker it started, as the AI
// Assistant's login status row does. The wait is bounded so a deadlock reads as
// a failure; a worker it gave up on is joined by the test once the lock is free.
class ThreadJoiningPanel : public GameEngine::DockPanel
{
public:
    explicit ThreadJoiningPanel(LoaderLockModel& model) : DockPanel("Joining"), m_Model(model)
    {
        m_Worker = std::thread(&ThreadJoiningPanel::RunWorker, m_Stop.get_future(),
                               std::ref(m_Model.LoaderLock), std::move(m_Exited));
    }
    ~ThreadJoiningPanel() override
    {
        m_Stop.set_value();
        if (m_ExitedFuture.wait_for(std::chrono::seconds(5)) == std::future_status::timeout)
        {
            m_Model.DestructorTimedOut = true;
            m_Model.Unjoined.push_back(std::move(m_Worker));
            return;
        }
        m_Worker.join();
    }

private:
    static void RunWorker(std::future<void> stop, std::mutex& loaderLock, std::promise<void> exited)
    {
        stop.wait();
        const std::lock_guard threadExit(loaderLock);
        exited.set_value();
    }

    LoaderLockModel& m_Model;
    std::promise<void> m_Stop;
    std::promise<void> m_Exited;
    std::future<void> m_ExitedFuture = m_Exited.get_future();
    std::thread m_Worker;
};

} // namespace

TEST(EditorPanelRegistry, SameIdRegistrationReplacesForHotReload)
{
    auto& registry = ed::EditorPanelRegistry::Get();

    registry.RegisterPanel(MakeDescriptor("sdkTest.panel.replace", "First"));
    registry.RegisterPanel(MakeDescriptor("sdkTest.panel.replace", "Second"));

    EXPECT_EQ(CountPanelsWithId(registry.PanelSnapshot(), "sdkTest.panel.replace"), 1u)
        << "same-id re-registration must not grow the registry";

    ed::EditorPanelDescriptor descriptor;
    ASSERT_TRUE(registry.TryGetPanel("sdkTest.panel.replace", descriptor));
    EXPECT_EQ(descriptor.Title, "Second") << "the replacement descriptor must win";

    // The replacement's FACTORY must win too — stale module factories must
    // stop being consumed after a reload.
    ASSERT_TRUE(descriptor.Factory);
    const std::unique_ptr<GameEngine::UIElement> panel = descriptor.Factory();
    auto* dockPanel = dynamic_cast<GameEngine::DockPanel*>(panel.get());
    ASSERT_NE(dockPanel, nullptr);
    EXPECT_EQ(dockPanel->GetTitle(), "Second");
}

TEST(EditorPanelRegistry, DescriptorRoundTripsIntact)
{
    auto& registry = ed::EditorPanelRegistry::Get();
    registry.RegisterPanel(MakeDescriptor("sdkTest.panel.roundtrip", "Round Trip"));

    ed::EditorPanelDescriptor descriptor;
    ASSERT_TRUE(registry.TryGetPanel("sdkTest.panel.roundtrip", descriptor));
    EXPECT_EQ(descriptor.Title, "Round Trip");
    EXPECT_EQ(descriptor.TabIconClass, "sdk-test-tab-icon");
    EXPECT_EQ(descriptor.AssetSourceAlias, "sdk-test-pack");
    EXPECT_EQ(descriptor.LayoutAssetPath, "UI/panels/SdkTest.uxml");
    EXPECT_EQ(descriptor.StyleAssetPath, "UI/panels/SdkTest.css");
    EXPECT_EQ(descriptor.DefaultDockAnchorPanelId, "SceneView");

    EXPECT_FALSE(registry.TryGetPanel("sdkTest.panel.doesNotExist", descriptor));
}

TEST(EditorPanelRegistry, RejectsDescriptorsWithoutIdOrFactory)
{
    auto& registry = ed::EditorPanelRegistry::Get();

    ed::EditorPanelDescriptor noId = MakeDescriptor("", "No Id");
    registry.RegisterPanel(std::move(noId));
    EXPECT_EQ(CountPanelsWithId(registry.PanelSnapshot(), ""), 0u);

    ed::EditorPanelDescriptor noFactory = MakeDescriptor("sdkTest.panel.noFactory", "No Factory");
    noFactory.Factory = nullptr;
    registry.RegisterPanel(std::move(noFactory));
    ed::EditorPanelDescriptor out;
    EXPECT_FALSE(registry.TryGetPanel("sdkTest.panel.noFactory", out));
}

TEST(EditorPanelRegistry, ConsumersReplayEarlierRegistrationsAndSeeLaterOnes)
{
    auto& registry = ed::EditorPanelRegistry::Get();
    ConsumerDetachGuard detach;

    // Module-first order (test hosts): registrations land before any consumer.
    registry.RegisterPanel(MakeDescriptor("sdkTest.panel.replay", "Replayed"));
    registry.RegisterEditorStyleSheet({"sdk-test-pack", "UI/SdkTestReplay.css"});

    std::vector<std::string> consumedPanels;
    std::vector<std::string> consumedSheets;
    ed::EditorPanelRegistry::Consumers consumers;
    consumers.Panel = [&](const ed::EditorPanelDescriptor& d) { consumedPanels.push_back(d.PanelId); };
    consumers.StyleSheet = [&](const ed::EditorStyleSheetContribution& c) {
        consumedSheets.push_back(c.AssetSourceAlias + ":" + c.StyleAssetPath);
    };
    registry.SetConsumers(std::move(consumers));

    const auto contains = [](const std::vector<std::string>& values, const std::string& v) {
        return std::find(values.begin(), values.end(), v) != values.end();
    };
    EXPECT_TRUE(contains(consumedPanels, "sdkTest.panel.replay"))
        << "attach must replay earlier panel registrations";
    EXPECT_TRUE(contains(consumedSheets, "sdk-test-pack:UI/SdkTestReplay.css"))
        << "attach must replay earlier stylesheet contributions";

    // Editor order: consumer already attached when the module registers.
    consumedPanels.clear();
    registry.RegisterPanel(MakeDescriptor("sdkTest.panel.live", "Live"));
    EXPECT_FALSE(contains(consumedPanels, "sdkTest.panel.live"))
        << "a registration must not reach the consumer inside the registering call";
    registry.DeliverPendingRegistrations();
    EXPECT_TRUE(contains(consumedPanels, "sdkTest.panel.live"));
}

// A package module reload registers its panel from DllMain, under the loader
// lock. If the registration destroys the panel it replaces there, a destructor
// that joins a thread waits on a thread that can only end by taking that lock:
// the editor hangs.
TEST(EditorPanelRegistry, ReplacedPanelIsDestroyedOutsideTheRegisteringCall)
{
    auto& registry = ed::EditorPanelRegistry::Get();
    ConsumerDetachGuard detach;
    LoaderLockModel model;

    // The editor's consumer: build the new panel, drop the one it replaces.
    std::vector<std::unique_ptr<GameEngine::UIElement>> openPanels;
    ed::EditorPanelRegistry::Consumers consumers;
    consumers.Panel = [&](const ed::EditorPanelDescriptor& d) {
        openPanels.clear();
        openPanels.push_back(d.Factory());
    };
    registry.SetConsumers(std::move(consumers));

    ed::EditorPanelDescriptor descriptor = MakeDescriptor("sdkTest.panel.joining", "Joining");
    descriptor.Factory = [&model]() -> std::unique_ptr<GameEngine::UIElement> {
        return std::make_unique<ThreadJoiningPanel>(model);
    };
    registry.RegisterPanel(descriptor);
    registry.DeliverPendingRegistrations();
    ASSERT_EQ(openPanels.size(), 1u);

    {
        const std::lock_guard dllMain(model.LoaderLock);
        registry.RegisterPanel(descriptor);
    }
    registry.DeliverPendingRegistrations();
    for (std::thread& worker : model.Unjoined)
        worker.join();

    EXPECT_FALSE(model.DestructorTimedOut)
        << "the replaced panel was destroyed inside the registration, under the loader lock";
    EXPECT_EQ(openPanels.size(), 1u);
    openPanels.clear();
}

// The editor delivers on every update: a registration reaches the consumer once,
// with the descriptor registered last, however often it was registered before the
// delivery, and never again on a later update.
TEST(EditorPanelRegistry, PendingPanelIsDeliveredOnceWithItsLatestDescriptor)
{
    auto& registry = ed::EditorPanelRegistry::Get();
    ConsumerDetachGuard detach;

    std::vector<std::string> deliveredTitles;
    ed::EditorPanelRegistry::Consumers consumers;
    consumers.Panel = [&](const ed::EditorPanelDescriptor& d) {
        if (d.PanelId == "sdkTest.panel.once")
            deliveredTitles.push_back(d.Title);
    };
    registry.SetConsumers(std::move(consumers));

    registry.RegisterPanel(MakeDescriptor("sdkTest.panel.once", "First"));
    registry.RegisterPanel(MakeDescriptor("sdkTest.panel.once", "Second"));
    registry.DeliverPendingRegistrations();
    EXPECT_EQ(deliveredTitles, std::vector<std::string>{"Second"})
        << "two registrations of one id before a delivery must reach the consumer once, with the last one";

    registry.DeliverPendingRegistrations();
    EXPECT_EQ(deliveredTitles.size(), 1u) << "a later update delivered the same registration again";
}

// Attaching consumers replays every registration, the pending ones included, so
// the next update must not deliver those a second time.
TEST(EditorPanelRegistry, AttachingConsumersDeliversAPendingRegistrationOnce)
{
    auto& registry = ed::EditorPanelRegistry::Get();
    ConsumerDetachGuard detach;

    registry.SetConsumers({.Panel = [](const ed::EditorPanelDescriptor&) {}});
    registry.RegisterPanel(MakeDescriptor("sdkTest.panel.attach", "Attach"));

    int deliveries = 0;
    ed::EditorPanelRegistry::Consumers consumers;
    consumers.Panel = [&](const ed::EditorPanelDescriptor& d) {
        if (d.PanelId == "sdkTest.panel.attach")
            ++deliveries;
    };
    registry.SetConsumers(std::move(consumers));
    EXPECT_EQ(deliveries, 1) << "attach must replay the pending registration";
    registry.DeliverPendingRegistrations();
    EXPECT_EQ(deliveries, 1) << "the update after the attach delivered the replayed registration again";
}

// Overlays and editor stylesheets register from the same DllMain as panels: their
// consumers run on the next delivery, never inside the registering call.
TEST(EditorPanelRegistry, OverlayAndStyleSheetRegisteredAfterAttachWaitForDelivery)
{
    auto& registry = ed::EditorPanelRegistry::Get();
    ConsumerDetachGuard detach;
    LoaderLockModel model;

    int overlays = 0;
    int styleSheets = 0;
    ed::EditorPanelRegistry::Consumers consumers;
    consumers.Overlay = [&](const ed::EditorOverlayDescriptor& d) {
        if (d.OverlayId == "sdkTest.overlay.deferred")
            ++overlays;
    };
    consumers.StyleSheet = [&](const ed::EditorStyleSheetContribution& c) {
        if (c.StyleAssetPath == "UI/SdkTestDeferred.css")
            ++styleSheets;
    };
    registry.SetConsumers(std::move(consumers));

    {
        const std::lock_guard dllMain(model.LoaderLock);
        registry.RegisterOverlay({"sdkTest.overlay.deferred",
                                  [] { return std::make_unique<GameEngine::DockPanel>("Deferred"); }});
        registry.RegisterEditorStyleSheet({"sdk-test-pack", "UI/SdkTestDeferred.css"});
        EXPECT_EQ(overlays, 0) << "the overlay consumer ran inside the registering call";
        EXPECT_EQ(styleSheets, 0) << "the stylesheet consumer ran inside the registering call";
    }
    registry.DeliverPendingRegistrations();
    EXPECT_EQ(overlays, 1);
    EXPECT_EQ(styleSheets, 1);
}

TEST(EditorPanelRegistry, StyleSheetContributionsDedupeButRenotify)
{
    auto& registry = ed::EditorPanelRegistry::Get();
    ConsumerDetachGuard detach;

    int notifications = 0;
    ed::EditorPanelRegistry::Consumers consumers;
    consumers.StyleSheet = [&](const ed::EditorStyleSheetContribution&) { ++notifications; };
    registry.SetConsumers(std::move(consumers));

    const auto countSheet = [&]() {
        const auto sheets = registry.StyleSheetSnapshot();
        return std::count_if(sheets.begin(), sheets.end(),
                             [](const ed::EditorStyleSheetContribution& c) {
                                 return c.AssetSourceAlias == "sdk-test-pack" &&
                                        c.StyleAssetPath == "UI/SdkTestDedupe.css";
                             });
    };

    const int baseline = notifications;
    registry.RegisterEditorStyleSheet({"sdk-test-pack", "UI/SdkTestDedupe.css"});
    registry.DeliverPendingRegistrations();
    registry.RegisterEditorStyleSheet({"sdk-test-pack", "UI/SdkTestDedupe.css"}); // module reload
    registry.DeliverPendingRegistrations();
    EXPECT_EQ(countSheet(), 1) << "re-registration must not duplicate the contribution";
    EXPECT_EQ(notifications, baseline + 2)
        << "a reloaded module must still re-notify (global attach is idempotent)";
}

TEST(EditorPanelRegistry, OverlaysReplaceForwardReplayAndNotifyLive)
{
    auto& registry = ed::EditorPanelRegistry::Get();
    ConsumerDetachGuard detach;

    // Module-first order: registration lands before any consumer.
    registry.RegisterOverlay({"sdkTest.overlay.replay",
                              [] { return std::make_unique<GameEngine::DockPanel>("Replayed"); }});

    std::vector<std::string> consumed;
    ed::EditorPanelRegistry::Consumers consumers;
    consumers.Overlay = [&](const ed::EditorOverlayDescriptor& d) { consumed.push_back(d.OverlayId); };
    registry.SetConsumers(std::move(consumers));
    EXPECT_NE(std::find(consumed.begin(), consumed.end(), "sdkTest.overlay.replay"), consumed.end())
        << "attach must replay earlier overlay registrations";

    // Editor order: consumer already attached when the module registers.
    consumed.clear();
    registry.RegisterOverlay({"sdkTest.overlay.live",
                              [] { return std::make_unique<GameEngine::DockPanel>("Live"); }});
    registry.DeliverPendingRegistrations();
    EXPECT_NE(std::find(consumed.begin(), consumed.end(), "sdkTest.overlay.live"), consumed.end());

    // Replace-forward: same-id re-registration must not grow the registry and
    // the fresh factory must win in the snapshot.
    registry.RegisterOverlay({"sdkTest.overlay.replay",
                              [] { return std::make_unique<GameEngine::DockPanel>("Second"); }});
    const auto overlays = registry.OverlaySnapshot();
    EXPECT_EQ(std::count_if(overlays.begin(), overlays.end(),
                            [](const ed::EditorOverlayDescriptor& d) {
                                return d.OverlayId == "sdkTest.overlay.replay";
                            }),
              1);
    const auto it = std::find_if(overlays.begin(), overlays.end(),
                                 [](const ed::EditorOverlayDescriptor& d) {
                                     return d.OverlayId == "sdkTest.overlay.replay";
                                 });
    ASSERT_NE(it, overlays.end());
    ASSERT_TRUE(it->Factory);
    const std::unique_ptr<GameEngine::UIElement> element = it->Factory();
    auto* dockPanel = dynamic_cast<GameEngine::DockPanel*>(element.get());
    ASSERT_NE(dockPanel, nullptr);
    EXPECT_EQ(dockPanel->GetTitle(), "Second");
}

TEST(EditorPanelRegistry, RejectsOverlaysWithoutIdOrFactory)
{
    auto& registry = ed::EditorPanelRegistry::Get();
    const auto countWithId = [&](std::string_view id) {
        const auto overlays = registry.OverlaySnapshot();
        return std::count_if(overlays.begin(), overlays.end(),
                             [&](const ed::EditorOverlayDescriptor& d) { return d.OverlayId == id; });
    };

    registry.RegisterOverlay({"", [] { return std::make_unique<GameEngine::DockPanel>("NoId"); }});
    EXPECT_EQ(countWithId(""), 0);

    registry.RegisterOverlay({"sdkTest.overlay.noFactory", nullptr});
    EXPECT_EQ(countWithId("sdkTest.overlay.noFactory"), 0);
}

TEST(EditorPanelRegistry, OpenPanelDispatchesThroughInstalledOpener)
{
    auto& registry = ed::EditorPanelRegistry::Get();

    // Loud no-op without an opener (module calling before editor install).
    registry.SetPanelOpener(nullptr);
    registry.OpenPanel("sdkTest.panel.opener");

    std::string opened;
    registry.SetPanelOpener([&](const std::string& panelId) { opened = panelId; });
    registry.OpenPanel("sdkTest.panel.opener");
    EXPECT_EQ(opened, "sdkTest.panel.opener");

    registry.SetPanelOpener(nullptr);
}

TEST(EditorPanelRegistry, PanelTypesBuildReplaceForwardAndRejectUnknownKeys)
{
    auto& registry = ed::EditorPanelRegistry::Get();

    int firstBuilds = 0;
    int secondBuilds = 0;
    registry.RegisterPanelType({"SdkTestPanelType", [&]() -> std::unique_ptr<GameEngine::UIElement> {
                                    ++firstBuilds;
                                    return std::make_unique<GameEngine::DockPanel>("first");
                                }});
    registry.RegisterPanelType({"SdkTestPanelType", [&]() -> std::unique_ptr<GameEngine::UIElement> {
                                    ++secondBuilds;
                                    return std::make_unique<GameEngine::DockPanel>("second");
                                }});

    EXPECT_NE(registry.CreatePanelOfType("SdkTestPanelType"), nullptr);
    EXPECT_EQ(firstBuilds, 0) << "the replaced factory must stop being called";
    EXPECT_EQ(secondBuilds, 1);

    EXPECT_EQ(registry.CreatePanelOfType("SdkTestNoSuchPanelType"), nullptr);

    registry.RegisterPanelType({"", [] { return std::unique_ptr<GameEngine::UIElement>{}; }});
    registry.RegisterPanelType({"SdkTestNoFactoryType", nullptr});
    EXPECT_EQ(registry.CreatePanelOfType("SdkTestNoFactoryType"), nullptr)
        << "a type without a factory must not enter the registry";
}
