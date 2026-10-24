#include "Logger/CallbackSink.h"
#include "Logger/Logger.h"
#include "UI/Controls/Mount.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <memory>

using namespace GameEngine;

namespace
{

std::unique_ptr<UIElement> MakeNamedElement(const char* id)
{
    auto element = std::make_unique<UIElement>();
    element->SetId(id);
    return element;
}

// Two different ids with the same 64-bit FNV-1a StringId (0x5e08d54d78217e0e).
constexpr const char* kCollidingId = "bf13eaba83dea434";
constexpr const char* kCollidingOtherId = "b3b828bb3655e2a7";
static_assert(HashStringId(kCollidingId) == HashStringId(kCollidingOtherId));

// Registers kCollidingId first, then refuses kCollidingOtherId, which is inserted before
// it in tree order: a lookup returns the registered element only if the refused one was
// never registered.
void AddCollidingIds(UIManager& manager)
{
    manager.SetRoot(MakeNamedElement("root"));
    manager.GetRootElement()->AddChild(MakeNamedElement(kCollidingId));
    manager.GetRootElement()->InsertChild(0, MakeNamedElement(kCollidingOtherId));
}

} // namespace

TEST(ElementIdLookup, TracksAttachDetachAndCrossManagerReparenting)
{
    UIManager first(nullptr);
    UIManager second(nullptr);
    first.SetRoot(MakeNamedElement("first"));
    second.SetRoot(MakeNamedElement("second"));
    auto child = MakeNamedElement("child");
    UIElement* childPointer = child.get();
    first.GetRootElement()->AddChild(std::move(child));
    EXPECT_EQ(first.GetRootElement()->FindById("child"_sid), childPointer);
    EXPECT_EQ(second.GetRootElement()->FindById("child"_sid), nullptr);

    child = first.GetRootElement()->TakeChild(childPointer);
    EXPECT_EQ(first.GetRootElement()->FindById("child"_sid), nullptr);
    EXPECT_EQ(child->FindById("child"_sid), childPointer);
    second.GetRootElement()->AddChild(std::move(child));
    EXPECT_EQ(second.GetRootElement()->FindById("child"_sid), childPointer);
    second.GetRootElement()->RemoveChild(childPointer);
    EXPECT_EQ(second.GetRootElement()->FindById("child"_sid), nullptr);
}

TEST(ElementIdLookup, RenamingAndClearingRemoveTheOldId)
{
    UIManager manager(nullptr);
    manager.SetRoot(MakeNamedElement("root"));
    auto child = MakeNamedElement("old");
    UIElement* childPointer = child.get();
    manager.GetRootElement()->AddChild(std::move(child));
    childPointer->SetId("new");
    EXPECT_EQ(manager.GetRootElement()->FindById("old"_sid), nullptr);
    EXPECT_EQ(manager.GetRootElement()->FindById("new"_sid), childPointer);
    childPointer->SetId("");
    EXPECT_EQ(manager.GetRootElement()->FindById("new"_sid), nullptr);
}

TEST(ElementIdLookup, DuplicateIdsUseTheFirstInTreeOrderInsideTheRequestedSubtree)
{
    UIManager manager(nullptr);
    manager.SetRoot(MakeNamedElement("root"));
    auto first = MakeNamedElement("duplicate");
    auto second = MakeNamedElement("duplicate");
    UIElement* firstPointer = first.get();
    UIElement* secondPointer = second.get();
    UIElement* root = manager.GetRootElement();
    root->AddChild(std::move(second));
    root->InsertChild(0, std::move(first));
    EXPECT_EQ(root->FindById("duplicate"_sid), firstPointer);
    EXPECT_EQ(firstPointer->FindById("duplicate"_sid), firstPointer);
    EXPECT_EQ(secondPointer->FindById("duplicate"_sid), secondPointer);

    secondPointer->SetId("duplicate");
    EXPECT_EQ(root->FindById("duplicate"_sid), firstPointer);
    secondPointer->SetId("temporary");
    secondPointer->SetId("duplicate");
    EXPECT_EQ(root->FindById("duplicate"_sid), firstPointer);
    second = root->TakeChild(secondPointer);
    root->AddChild(std::move(second));
    EXPECT_EQ(root->FindById("duplicate"_sid), firstPointer);
    first = root->TakeChild(firstPointer);
    root->AddChild(std::move(first));
    EXPECT_EQ(root->FindById("duplicate"_sid), secondPointer);
    root->RemoveChild(secondPointer);
    EXPECT_EQ(root->FindById("duplicate"_sid), firstPointer);
}

TEST(ElementIdLookup, DuplicateAncestorsPrecedeDescendantsAndEarlierBranches)
{
    UIManager manager(nullptr);
    manager.SetRoot(MakeNamedElement("duplicate"));
    UIElement* root = manager.GetRootElement();
    auto branch = MakeNamedElement("branch");
    UIElement* branchPointer = branch.get();
    auto descendant = MakeNamedElement("duplicate");
    UIElement* descendantPointer = descendant.get();
    branch->AddChild(std::move(descendant));
    root->AddChild(std::move(branch));
    root->AddChild(MakeNamedElement("duplicate"));
    EXPECT_EQ(root->FindById("duplicate"_sid), root);
    root->SetId("root");
    EXPECT_EQ(root->FindById("duplicate"_sid), descendantPointer);
    branchPointer->SetId("duplicate");
    EXPECT_EQ(root->FindById("duplicate"_sid), branchPointer);
    descendantPointer->SetId("temporary");
    descendantPointer->SetId("duplicate");
    EXPECT_EQ(branchPointer->FindById("duplicate"_sid), branchPointer);
}

TEST(ElementIdLookup, DuplicateMountTargetsFollowOrdinaryChildren)
{
    auto target = MakeNamedElement("duplicate");
    UIManager manager(nullptr);
    auto mount = std::make_unique<Mount>();
    Mount* mountPointer = mount.get();
    auto child = MakeNamedElement("duplicate");
    UIElement* childPointer = child.get();
    mount->AddChild(std::move(child));
    manager.SetRoot(std::move(mount));
    mountPointer->SetTarget(target.get());
    EXPECT_EQ(mountPointer->FindById("duplicate"_sid), childPointer);
    childPointer->SetId("temporary");
    childPointer->SetId("duplicate");
    EXPECT_EQ(mountPointer->FindById("duplicate"_sid), childPointer);
    mountPointer->RemoveChild(childPointer);
    EXPECT_EQ(mountPointer->FindById("duplicate"_sid), target.get());
}

TEST(ElementIdLookup, DoesNotReturnAnElementOutsideTheRequestedSubtree)
{
    UIManager manager(nullptr);
    manager.SetRoot(MakeNamedElement("root"));
    auto branch = MakeNamedElement("branch");
    UIElement* branchPointer = branch.get();
    manager.GetRootElement()->AddChild(std::move(branch));
    manager.GetRootElement()->AddChild(MakeNamedElement("sibling"));
    EXPECT_EQ(branchPointer->FindById("sibling"_sid), nullptr);
    EXPECT_EQ(branchPointer->FindById("root"_sid), nullptr);
}

TEST(ElementIdLookup, MountRetargetingAndActivationAreImmediatelyVisible)
{
    auto first = MakeNamedElement("first-panel");
    first->AddChild(MakeNamedElement("first-content"));
    auto second = MakeNamedElement("second-panel");
    second->AddChild(MakeNamedElement("second-content"));
    UIManager manager(nullptr);
    auto root = MakeNamedElement("root");
    auto mount = std::make_unique<Mount>();
    Mount* mountPointer = mount.get();
    mount->SetTarget(first.get());
    root->AddChild(std::move(mount));
    manager.SetRoot(std::move(root));
    EXPECT_NE(manager.GetRootElement()->FindById("first-content"_sid), nullptr);

    mountPointer->SetTarget(second.get());
    EXPECT_EQ(manager.GetRootElement()->FindById("first-content"_sid), nullptr);
    EXPECT_NE(manager.GetRootElement()->FindById("second-content"_sid), nullptr);
    mountPointer->SwapTargetForActivation(first.get());
    EXPECT_NE(manager.GetRootElement()->FindById("first-content"_sid), nullptr);
    EXPECT_EQ(manager.GetRootElement()->FindById("second-content"_sid), nullptr);
    mountPointer->SetTarget(nullptr);
    EXPECT_EQ(manager.GetRootElement()->FindById("first-content"_sid), nullptr);
}

TEST(ElementIdLookup, DestroyedMountTargetsAndReplacedRootsLeaveNoResults)
{
    auto target = MakeNamedElement("target");
    UIManager manager(nullptr);
    auto mount = std::make_unique<Mount>();
    mount->SetTarget(target.get());
    manager.SetRoot(std::move(mount));
    EXPECT_EQ(manager.GetRootElement()->FindById("target"_sid), target.get());
    target.reset();
    EXPECT_EQ(manager.GetRootElement()->FindById("target"_sid), nullptr);
    manager.SetRoot(MakeNamedElement("replacement"));
    EXPECT_EQ(manager.GetRootElement()->FindById("target"_sid), nullptr);
    EXPECT_EQ(manager.GetRootElement()->FindById("replacement"_sid), manager.GetRootElement());
}

TEST(ElementIdLookup, AStringIdCollisionIsReportedAtRegistrationNamingBothIds)
{
#ifndef NDEBUG
#if GTEST_HAS_DEATH_TEST
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            Logger::Log::Initialize({});
            auto sink = std::make_unique<Logger::CallbackSink>();
            sink->RegisterCallback([](const Logger::LogMessage& message)
                                   { std::fprintf(stderr, "%s\n", message.Message.c_str()); });
            Logger::Log::AddSink(std::move(sink));
            UIManager manager(nullptr);
            AddCollidingIds(manager);
        },
        "bf13eaba83dea434.*b3b828bb3655e2a7");
#else
    GTEST_SKIP() << "death tests not supported on this platform";
#endif
#else
    Logger::Log::Initialize({});
    auto sink = std::make_unique<Logger::CallbackSink>();
    auto* sinkPointer = sink.get();
    auto reports = std::make_shared<std::atomic<int>>(0);
    const std::uint64_t callbackId = sinkPointer->RegisterCallback(
        [reports](const Logger::LogMessage& message)
        {
            if (message.Message.find(kCollidingId) != std::string::npos &&
                message.Message.find(kCollidingOtherId) != std::string::npos)
                reports->fetch_add(1);
        });
    Logger::Log::AddSink(std::move(sink));

    UIManager manager(nullptr);
    AddCollidingIds(manager);
    Logger::Log::Flush();
    sinkPointer->UnregisterCallback(callbackId);

    EXPECT_EQ(reports->load(), 1);
    UIElement* first = manager.GetRootElement()->FindById(HashStringId(kCollidingId));
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->GetId(), kCollidingId) << "the refused id must not alias the registered one";
#endif
}

TEST(ElementIdLookup, AnEmptyIdFindsNothing)
{
    UIManager manager(nullptr);
    manager.SetRoot(std::make_unique<UIElement>());
    manager.GetRootElement()->AddChild(std::make_unique<UIElement>());
    EXPECT_EQ(manager.GetRootElement()->FindById(0), nullptr);
    EXPECT_EQ(manager.GetRootElement()->FindById(HashStringId("")), nullptr);

    UIElement offline;
    offline.AddChild(std::make_unique<UIElement>());
    EXPECT_EQ(offline.FindById(0), nullptr);
}
