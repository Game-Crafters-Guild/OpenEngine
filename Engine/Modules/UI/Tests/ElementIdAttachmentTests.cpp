#include "UI/Controls/Mount.h"
#include "UI/Internal/AttachedRootAccess.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace GameEngine;

namespace
{

constexpr std::array<const char*, 8> kIds = {"a", "b", "c", "d", "e", "f", "g", "h"};

std::unique_ptr<UIElement> MakeNamedElement(const char* id)
{
    auto element = std::make_unique<UIElement>();
    element->SetId(id);
    return element;
}

// What UIElement::FindById must return, found the slow way: depth-first from `node` along
// the DFS edges (children whose DFS parent is the node, in order, then its Mount target),
// the first element `manager` owns that carries `id`.
UIElement* FindByWalking(UIElement& node, StringId id, const UIManager* manager)
{
    if (node.GetIdHash() == id && node.GetOwnerManager() == manager)
        return &node;
    for (const auto& child : node.GetChildren())
    {
        if (child && child->GetDfsParent() == &node)
        {
            if (UIElement* found = FindByWalking(*child, id, manager))
                return found;
        }
    }
    if (UIElement* target = node.GetMountTarget(); target && target->GetDfsParent() == &node)
        return FindByWalking(*target, id, manager);
    return nullptr;
}

void CollectByOwnership(UIElement& node, std::vector<UIElement*>& out)
{
    out.push_back(&node);
    for (const auto& child : node.GetChildren())
        CollectByOwnership(*child, out);
}

bool IsOnDfsChain(const UIElement* from, const UIElement* element)
{
    for (const UIElement* node = from; node; node = node->GetDfsParent())
    {
        if (node == element)
            return true;
    }
    return false;
}

bool IsOnOwnershipChain(const UIElement* from, const UIElement* element)
{
    for (const UIElement* node = from; node; node = node->GetParent())
    {
        if (node == element)
            return true;
    }
    return false;
}

// Two managers and a pool of detached elements the test owns, mutated at random through
// every public tree edit, and checked against FindByWalking after each one.
//
// Mount targets are always pool tops (no parent) and a mounted element is never given a
// parent, so releasing a target can never close a DFS cycle; every edit that adds a DFS
// edge is refused when it would close one.
class TreeMutator
{
  public:
    explicit TreeMutator(uint32_t seed) : m_Random(seed)
    {
        for (auto& manager : m_Managers)
        {
            manager = std::make_unique<UIManager>(nullptr);
            manager->SetRoot(NewElement());
        }
    }

    void Step()
    {
        std::vector<UIElement*> live = LiveElements();
        const bool crowded = live.size() > kElementBudget;
        switch (Pick(crowded ? 4u : 12u))
        {
        case 0: RemoveRandomChild(live); break;
        case 1: DropRandomPoolElement(); break;
        case 2: TakeRandomChild(live); break;
        case 3: RetargetRandomMount(live); break;
        case 4: RenameRandomElement(live); break;
        case 5: MoveRandomChild(live); break;
        case 6: ReplaceRandomRoot(); break;
        case 7: if (Pick(8u) == 0) ReplaceSecondManager(); break;
        default: AddRandomElement(live); break;
        }
    }

    // Every element's attached root against the top of its DFS-parent chain. Lookups alone
    // cannot see this: a root whose own attached root is wrong falls back to the parent
    // walk and still answers correctly, only slower.
    void ExpectAttachedRootsMatchTheChain(int step)
    {
        for (UIElement* element : LiveElements())
        {
            const UIElement* top = element;
            while (top->GetDfsParent())
                top = top->GetDfsParent();
            const UIElement* expected = nullptr;
            for (const auto& manager : m_Managers)
            {
                if (manager->GetRootElement() == top)
                    expected = top;
            }
            ASSERT_EQ(UIAttachedRootAccess::AttachedRoot(*element), expected)
                << "step " << step << ", element '" << element->GetId() << "'";
        }
    }

    // Every lookup from every manager root, and from a sample of owned elements anywhere
    // (attached, in a detached owned subtree, inside a mounted target), against the walk.
    void ExpectLookupsMatchTheWalk(int step)
    {
        for (const auto& manager : m_Managers)
        {
            UIElement* root = manager->GetRootElement();
            ExpectLookupsFrom(*root, step);
        }
        std::vector<UIElement*> live = LiveElements();
        for (int sample = 0; sample < kSubtreeSamples && !live.empty(); ++sample)
        {
            UIElement* subtree = live[Pick(static_cast<uint32_t>(live.size()))];
            if (subtree->GetOwnerManager())
                ExpectLookupsFrom(*subtree, step);
        }
    }

  private:
    static constexpr size_t kElementBudget = 90;
    static constexpr int kSubtreeSamples = 6;

    uint32_t Pick(uint32_t count) { return static_cast<uint32_t>(m_Random() % count); }

    std::unique_ptr<UIElement> NewElement()
    {
        std::unique_ptr<UIElement> element =
            Pick(5u) == 0 ? std::unique_ptr<UIElement>(std::make_unique<Mount>()) : std::make_unique<UIElement>();
        if (Pick(4u) != 0)
            element->SetId(kIds[Pick(static_cast<uint32_t>(kIds.size()))]);
        return element;
    }

    std::vector<UIElement*> LiveElements()
    {
        std::vector<UIElement*> live;
        for (const auto& manager : m_Managers)
            CollectByOwnership(*manager->GetRootElement(), live);
        for (const auto& element : m_Pool)
            CollectByOwnership(*element, live);
        return live;
    }

    std::vector<Mount*> LiveMounts(const std::vector<UIElement*>& live)
    {
        std::vector<Mount*> mounts;
        for (UIElement* element : live)
        {
            if (auto* mount = dynamic_cast<Mount*>(element))
                mounts.push_back(mount);
        }
        return mounts;
    }

    UIElement* PickWithParent(const std::vector<UIElement*>& live)
    {
        std::vector<UIElement*> children;
        for (UIElement* element : live)
        {
            if (element->GetParent())
                children.push_back(element);
        }
        return children.empty() ? nullptr : children[Pick(static_cast<uint32_t>(children.size()))];
    }

    // A pool element no Mount shows, so giving it a parent cannot leave it with two DFS
    // parents; nullptr when there is none.
    size_t PickUnmountedPoolIndex()
    {
        std::vector<size_t> unmounted;
        for (size_t i = 0; i < m_Pool.size(); ++i)
        {
            if (!m_Pool[i]->GetDfsParent())
                unmounted.push_back(i);
        }
        return unmounted.empty() ? m_Pool.size() : unmounted[Pick(static_cast<uint32_t>(unmounted.size()))];
    }

    void Attach(UIElement& parent, std::unique_ptr<UIElement> child)
    {
        if (Pick(2u) == 0)
            parent.AddChild(std::move(child));
        else
            parent.InsertChild(Pick(static_cast<uint32_t>(parent.GetChildren().size() + 1)), std::move(child));
    }

    void AddRandomElement(const std::vector<UIElement*>& live)
    {
        UIElement* parent = live[Pick(static_cast<uint32_t>(live.size()))];
        const size_t poolIndex = Pick(3u) == 0 ? PickUnmountedPoolIndex() : m_Pool.size();
        if (poolIndex == m_Pool.size())
        {
            Attach(*parent, NewElement());
            return;
        }
        UIElement* child = m_Pool[poolIndex].get();
        if (IsOnDfsChain(parent, child) || IsOnOwnershipChain(parent, child))
            return;
        std::unique_ptr<UIElement> owned = std::move(m_Pool[poolIndex]);
        m_Pool.erase(m_Pool.begin() + static_cast<std::ptrdiff_t>(poolIndex));
        Attach(*parent, std::move(owned));
    }

    void TakeRandomChild(const std::vector<UIElement*>& live)
    {
        if (UIElement* child = PickWithParent(live))
            m_Pool.push_back(child->GetParent()->TakeChild(child));
    }

    void RemoveRandomChild(const std::vector<UIElement*>& live)
    {
        if (UIElement* child = PickWithParent(live))
            child->GetParent()->RemoveChild(child);
    }

    // Reparent within or across trees in one step.
    void MoveRandomChild(const std::vector<UIElement*>& live)
    {
        UIElement* child = PickWithParent(live);
        UIElement* parent = live[Pick(static_cast<uint32_t>(live.size()))];
        if (!child || child->GetDfsParent() != child->GetParent() || IsOnOwnershipChain(parent, child) ||
            IsOnDfsChain(parent, child))
            return;
        Attach(*parent, child->GetParent()->TakeChild(child));
    }

    void DropRandomPoolElement()
    {
        if (!m_Pool.empty())
            m_Pool.erase(m_Pool.begin() + static_cast<std::ptrdiff_t>(Pick(static_cast<uint32_t>(m_Pool.size()))));
    }

    void RenameRandomElement(const std::vector<UIElement*>& live)
    {
        UIElement* element = live[Pick(static_cast<uint32_t>(live.size()))];
        element->SetId(Pick(6u) == 0 ? "" : kIds[Pick(static_cast<uint32_t>(kIds.size()))]);
    }

    void RetargetRandomMount(const std::vector<UIElement*>& live)
    {
        const std::vector<Mount*> mounts = LiveMounts(live);
        if (mounts.empty())
            return;
        Mount* mount = mounts[Pick(static_cast<uint32_t>(mounts.size()))];
        UIElement* target = nullptr;
        if (!m_Pool.empty() && Pick(4u) != 0)
            target = m_Pool[Pick(static_cast<uint32_t>(m_Pool.size()))].get();
        if (target && IsOnDfsChain(mount, target))
            return;
        if (Pick(2u) == 0)
            mount->SetTarget(target);
        else
            mount->SwapTargetForActivation(target);
    }

    // Root swap, with a fresh element or with an unmounted pool element.
    void ReplaceRandomRoot()
    {
        UIManager& manager = *m_Managers[Pick(static_cast<uint32_t>(m_Managers.size()))];
        const size_t poolIndex = PickUnmountedPoolIndex();
        if (poolIndex == m_Pool.size() || Pick(2u) == 0)
        {
            manager.SetRoot(NewElement());
            return;
        }
        std::unique_ptr<UIElement> root = std::move(m_Pool[poolIndex]);
        m_Pool.erase(m_Pool.begin() + static_cast<std::ptrdiff_t>(poolIndex));
        manager.SetRoot(std::move(root));
    }

    // A manager destroyed under elements that outlive it (pool elements it owns).
    void ReplaceSecondManager()
    {
        m_Managers[1] = std::make_unique<UIManager>(nullptr);
        m_Managers[1]->SetRoot(NewElement());
    }

    void ExpectLookupsFrom(UIElement& subtree, int step)
    {
        for (const char* id : kIds)
        {
            const StringId hash = HashStringId(id);
            ASSERT_EQ(subtree.FindById(hash), FindByWalking(subtree, hash, subtree.GetOwnerManager()))
                << "step " << step << ", id '" << id << "', from '" << subtree.GetId() << "'";
        }
    }

    std::mt19937 m_Random;
    std::array<std::unique_ptr<UIManager>, 2> m_Managers;
    std::vector<std::unique_ptr<UIElement>> m_Pool;
};

// What a LookupOnDetachProbe saw; kept outside the probe so it can be read after
// RemoveChild has destroyed the probe.
struct DetachLookupRecord
{
    bool Searched = false;
    bool Found = false;
};

// On losing its owner, looks `SearchId` up from `SearchRoot`: at that moment its own
// subtree is still owned and registered, but no longer under the root.
class LookupOnDetachProbe final : public UIElement
{
  public:
    LookupOnDetachProbe(DetachLookupRecord& record, UIElement& searchRoot, StringId searchId)
        : m_Record(record), m_SearchRoot(searchRoot), m_SearchId(searchId)
    {
    }

  protected:
    void OnOwnerManagerChanged(UIManager* owner) override
    {
        if (owner)
            return;
        m_Record.Searched = true;
        m_Record.Found = m_SearchRoot.FindById(m_SearchId) != nullptr;
    }

  private:
    DetachLookupRecord& m_Record;
    UIElement& m_SearchRoot;
    StringId m_SearchId;
};

// Attaches a probe with one named child under `root`, removes it with `remove`, and
// returns what the probe's lookup of that child from `root` saw.
template <typename RemoveFn>
DetachLookupRecord LookUpDescendantWhileDetaching(UIElement& root, RemoveFn remove)
{
    DetachLookupRecord record;
    auto probe = std::make_unique<LookupOnDetachProbe>(record, root, "descendant"_sid);
    LookupOnDetachProbe* probePointer = probe.get();
    probe->AddChild(MakeNamedElement("descendant"));
    root.AddChild(std::move(probe));
    EXPECT_NE(root.FindById("descendant"_sid), nullptr);
    remove(*probePointer);
    return record;
}

// On gaining an owner, takes `ancestor` out of its parent into `rescued`: the callback runs
// while the element is being attached and is not yet in its parent's children.
class DetachAncestorOnAttachProbe final : public UIElement
{
  public:
    DetachAncestorOnAttachProbe(UIElement& ancestor, std::unique_ptr<UIElement>& rescued)
        : m_Ancestor(&ancestor), m_Rescued(rescued)
    {
    }

  protected:
    void OnOwnerManagerChanged(UIManager* owner) override
    {
        if (!owner || !m_Ancestor || !m_Ancestor->GetParent())
            return;
        UIElement* ancestor = m_Ancestor;
        m_Ancestor = nullptr;
        m_Rescued = ancestor->GetParent()->TakeChild(ancestor);
    }

  private:
    UIElement* m_Ancestor;
    std::unique_ptr<UIElement>& m_Rescued;
};

// On gaining an owner, records whether a lookup of its own id from `searchRoot` finds it.
class LookupOnAttachProbe final : public UIElement
{
  public:
    LookupOnAttachProbe(UIElement& searchRoot, bool& searched, bool& found)
        : m_SearchRoot(searchRoot), m_Searched(searched), m_Found(found)
    {
    }

  protected:
    void OnOwnerManagerChanged(UIManager* owner) override
    {
        if (!owner)
            return;
        m_Searched = true;
        m_Found = m_SearchRoot.FindById(GetIdHash()) == this;
    }

  private:
    UIElement& m_SearchRoot;
    bool& m_Searched;
    bool& m_Found;
};

void AttachByAdd(UIElement& parent, std::unique_ptr<UIElement> child)
{
    parent.AddChild(std::move(child));
}

void AttachByInsert(UIElement& parent, std::unique_ptr<UIElement> child)
{
    parent.InsertChild(0, std::move(child));
}

using AttachFn = void (*)(UIElement&, std::unique_ptr<UIElement>);

// root > holder > parent; attaching a probe under `parent` with `attach` takes `holder` out
// of the root from the probe's owner callback. The probe must end up out of the root.
void ExpectAnAncestorDetachedDuringTheAttachTakesTheElementOut(AttachFn attach)
{
    UIManager manager(nullptr);
    manager.SetRoot(MakeNamedElement("root"));
    UIElement* root = manager.GetRootElement();
    auto holder = MakeNamedElement("holder");
    UIElement* holderPointer = holder.get();
    auto parent = MakeNamedElement("parent");
    UIElement* parentPointer = parent.get();
    holder->AddChild(std::move(parent));
    root->AddChild(std::move(holder));

    std::unique_ptr<UIElement> rescued;
    auto probe = std::make_unique<DetachAncestorOnAttachProbe>(*holderPointer, rescued);
    probe->SetId("attaching");
    UIElement* probePointer = probe.get();
    attach(*parentPointer, std::move(probe));

    ASSERT_EQ(rescued.get(), holderPointer);
    EXPECT_EQ(probePointer->GetParent(), parentPointer);
    EXPECT_EQ(UIAttachedRootAccess::AttachedRoot(*probePointer), nullptr);
    EXPECT_EQ(root->FindById("attaching"_sid), nullptr);
}

void ExpectTheRootFindsTheAttachingElementFromItsOwnerCallback(AttachFn attach)
{
    UIManager manager(nullptr);
    manager.SetRoot(MakeNamedElement("root"));
    UIElement* root = manager.GetRootElement();
    bool searched = false;
    bool found = false;
    auto probe = std::make_unique<LookupOnAttachProbe>(*root, searched, found);
    probe->SetId("attaching");
    attach(*root, std::move(probe));
    ASSERT_TRUE(searched);
    EXPECT_TRUE(found);
}

} // namespace

TEST(ElementIdAttachment, EveryLookupMatchesATreeWalkAfterEveryEdit)
{
    TreeMutator mutator(20261013u);
    for (int step = 0; step < 3000; ++step)
    {
        mutator.Step();
        mutator.ExpectAttachedRootsMatchTheChain(step);
        if (HasFatalFailure())
            return;
        mutator.ExpectLookupsMatchTheWalk(step);
        if (HasFatalFailure())
            return;
    }
}

TEST(ElementIdAttachment, ARemovedSubtreeIsOutOfTheRootBeforeItsOwnerIsCleared)
{
    UIManager manager(nullptr);
    manager.SetRoot(MakeNamedElement("root"));
    UIElement& root = *manager.GetRootElement();

    const DetachLookupRecord removed =
        LookUpDescendantWhileDetaching(root, [&root](UIElement& probe) { root.RemoveChild(&probe); });
    ASSERT_TRUE(removed.Searched);
    EXPECT_FALSE(removed.Found);

    std::unique_ptr<UIElement> taken;
    const DetachLookupRecord detached =
        LookUpDescendantWhileDetaching(root, [&](UIElement& probe) { taken = root.TakeChild(&probe); });
    ASSERT_TRUE(detached.Searched);
    EXPECT_FALSE(detached.Found);
}

TEST(ElementIdAttachment, AChildShownThroughAnotherMountFollowsTheMount)
{
    UIManager manager(nullptr);
    manager.SetRoot(MakeNamedElement("root"));
    UIElement* root = manager.GetRootElement();
    auto shown = MakeNamedElement("shown");
    UIElement* shownPointer = shown.get();
    auto holder = MakeNamedElement("holder");
    UIElement* holderPointer = holder.get();
    holder->AddChild(std::move(shown));

    // The detached Mount takes the child's DFS edge; attaching its parent afterwards must
    // not attach it.
    auto detachedHost = std::make_unique<Mount>();
    detachedHost->SetTarget(shownPointer);
    root->AddChild(std::move(holder));
    EXPECT_EQ(root->FindById("holder"_sid), holderPointer);
    EXPECT_EQ(root->FindById("shown"_sid), nullptr);

    // Released by the Mount, the child follows its parent again.
    detachedHost->SetTarget(nullptr);
    EXPECT_EQ(root->FindById("shown"_sid), shownPointer);
}

TEST(ElementIdAttachment, AnAncestorDetachedFromAnAttachCallbackTakesTheAttachingElementOut)
{
    {
        SCOPED_TRACE("AddChild");
        ExpectAnAncestorDetachedDuringTheAttachTakesTheElementOut(&AttachByAdd);
    }
    {
        SCOPED_TRACE("InsertChild");
        ExpectAnAncestorDetachedDuringTheAttachTakesTheElementOut(&AttachByInsert);
    }
}

TEST(ElementIdAttachment, TheRootFindsAnAttachingElementFromItsOwnerCallback)
{
    {
        SCOPED_TRACE("AddChild");
        ExpectTheRootFindsTheAttachingElementFromItsOwnerCallback(&AttachByAdd);
    }
    {
        SCOPED_TRACE("InsertChild");
        ExpectTheRootFindsTheAttachingElementFromItsOwnerCallback(&AttachByInsert);
    }
}
