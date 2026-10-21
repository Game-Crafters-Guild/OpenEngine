#include "PhysicsECS/PhysicsEntityEventCallbacks.h"

#include "PhysicsECS/PhysicsWorldService.h"

#include <deque>
#include <unordered_map>
#include <vector>

namespace GameEngine::PhysicsECS
{
namespace
{
using SubscriptionId = PhysicsEntityEventCallbacks::SubscriptionId;

enum class EntryKind : uint8
{
    ContactBegin,
    ContactEnd,
    TriggerEnter,
    TriggerExit,
};

struct Entry
{
    SubscriptionId id = 0;
    ECS::EntityHandle entity{};
    EntryKind kind = EntryKind::ContactBegin;
    // False once the subscription has ended; an ended entry is kept only until the delivery that may
    // be running its callback returns.
    bool live = true;

    PhysicsEntityEventCallbacks::ContactCallback contactCb{};
    PhysicsEntityEventCallbacks::TriggerCallback triggerCb{};
    PhysicsEntityEventCallbacks::EndedCallback ended{};
};

struct PerEntityLists
{
    std::vector<SubscriptionId> contactBegin;
    std::vector<SubscriptionId> contactEnd;
    std::vector<SubscriptionId> triggerEnter;
    std::vector<SubscriptionId> triggerExit;
};

static std::unordered_map<ECS::EntityHandle, PerEntityLists, ECS::EntityHandleHash> g_byEntity;
static std::unordered_map<SubscriptionId, Entry> g_entries;
static SubscriptionId g_nextId = 1;

static Physics::PhysicsWorld::ListenerId g_entityContactListenerId = 0;
static Physics::PhysicsWorld::ListenerId g_entityTriggerListenerId = 0;

// The deliveries running now (a callback that steps the world nests one). While any runs, the world
// iterates its listeners, so they are not removed, and an ended entry is not erased, because its
// callback may be the one running.
static uint32 g_deliveryDepth = 0;
// One id list per delivery level, reused, so a delivery allocates nothing once warm. A deque, so a
// nested level never moves the list an outer level iterates.
static std::deque<std::vector<SubscriptionId>> g_deliverySnapshots;
static std::vector<SubscriptionId> g_endedDuringDelivery;

static ECS::EntityHandle EntityRouterEntityFromUserData(uint64 ud)
{
    // Convention: PhysicsECS stores ECS entity id in userData.
    return ECS::EntityHandle(static_cast<uint32>(ud));
}

static std::vector<SubscriptionId>& ListOf(PerEntityLists& lists, EntryKind kind)
{
    switch (kind)
    {
    case EntryKind::ContactBegin:
        return lists.contactBegin;
    case EntryKind::ContactEnd:
        return lists.contactEnd;
    case EntryKind::TriggerEnter:
        return lists.triggerEnter;
    case EntryKind::TriggerExit:
        break;
    }
    return lists.triggerExit;
}

// Calls the live callbacks of `ids` with `ev`. The ids are copied first: a callback may subscribe or
// unsubscribe, which changes the list.
template <class Event, class Callback>
static void Deliver(const std::vector<SubscriptionId>& ids, const Event& ev, Callback Entry::*callback)
{
    if (ids.empty())
        return;
    if (g_deliverySnapshots.size() <= g_deliveryDepth)
        g_deliverySnapshots.emplace_back();
    std::vector<SubscriptionId>& snapshot = g_deliverySnapshots[g_deliveryDepth];
    snapshot.assign(ids.begin(), ids.end());

    ++g_deliveryDepth;
    for (const SubscriptionId id : snapshot)
    {
        const auto it = g_entries.find(id);
        if (it != g_entries.end() && it->second.live && it->second.*callback)
            (it->second.*callback)(ev);
    }
    --g_deliveryDepth;

    if (g_deliveryDepth == 0)
    {
        for (const SubscriptionId id : g_endedDuringDelivery)
            g_entries.erase(id);
        g_endedDuringDelivery.clear();
    }
}

// `normal` points from other into self.
static void DeliverContactTo(ECS::EntityHandle self, ECS::EntityHandle other, uint64 selfUserData, uint64 otherUserData,
                             const Physics::Vector3& normal, const Physics::ContactEvent& e)
{
    const auto it = g_byEntity.find(self);
    if (it == g_byEntity.end())
        return;
    EntityContactEvent ev{};
    ev.self = self;
    ev.other = other;
    ev.state = e.state;
    ev.point = e.point;
    ev.normal = normal;
    ev.impulse = e.impulse;
    ev.selfUserData = selfUserData;
    ev.otherUserData = otherUserData;
    const bool isBegin = e.state == Physics::ContactState::Begin;
    Deliver(isBegin ? it->second.contactBegin : it->second.contactEnd, ev, &Entry::contactCb);
}

static void OnWorldContact(const Physics::ContactEvent& e)
{
    const ECS::EntityHandle a = EntityRouterEntityFromUserData(e.userDataA);
    const ECS::EntityHandle b = EntityRouterEntityFromUserData(e.userDataB);
    if (!a.IsValid() || !b.IsValid())
        return;
    if (e.state != Physics::ContactState::Begin && e.state != Physics::ContactState::End)
        return;
    // The backend's normal points from body A into body B.
    DeliverContactTo(a, b, e.userDataA, e.userDataB, e.normal * -1.0f, e);
    DeliverContactTo(b, a, e.userDataB, e.userDataA, e.normal, e);
}

static void DeliverTriggerTo(ECS::EntityHandle self, ECS::EntityHandle other, bool selfIsTrigger, uint64 selfUserData,
                             uint64 otherUserData, bool isEntering)
{
    const auto it = g_byEntity.find(self);
    if (it == g_byEntity.end())
        return;
    EntityTriggerEvent ev{};
    ev.self = self;
    ev.other = other;
    ev.selfIsTrigger = selfIsTrigger;
    ev.isEntering = isEntering;
    ev.selfUserData = selfUserData;
    ev.otherUserData = otherUserData;
    Deliver(isEntering ? it->second.triggerEnter : it->second.triggerExit, ev, &Entry::triggerCb);
}

static void OnWorldTrigger(const Physics::TriggerEvent& e)
{
    const ECS::EntityHandle trigger = EntityRouterEntityFromUserData(e.triggerUserData);
    const ECS::EntityHandle other = EntityRouterEntityFromUserData(e.otherUserData);
    if (!trigger.IsValid() || !other.IsValid())
        return;
    DeliverTriggerTo(trigger, other, true, e.triggerUserData, e.otherUserData, e.isEntering);
    DeliverTriggerTo(other, trigger, false, e.otherUserData, e.triggerUserData, e.isEntering);
}

// The two world listeners are installed together while any subscription exists, so a callback that
// subscribes to the other kind during a delivery finds its listener in place and never changes the
// world's listener lists while the world iterates them.
static void EnsureWorldListeners()
{
    if (!PhysicsWorldService::IsInitialized() || g_byEntity.empty())
        return;

    auto& w = PhysicsWorldService::Get();
    if (g_entityContactListenerId == 0)
        g_entityContactListenerId = w.AddContactListener(&OnWorldContact);
    if (g_entityTriggerListenerId == 0)
        g_entityTriggerListenerId = w.AddTriggerListener(&OnWorldTrigger);
}

// Removes the world listeners when no subscription is left and no delivery runs. When the last
// subscription ends during a delivery, the listeners stay until the next end call outside one.
static void MaybeRemoveWorldListeners()
{
    if (!PhysicsWorldService::IsInitialized())
    {
        g_entityContactListenerId = 0;
        g_entityTriggerListenerId = 0;
        return;
    }
    if (g_deliveryDepth != 0 || !g_byEntity.empty())
        return;

    auto& w = PhysicsWorldService::Get();
    if (g_entityContactListenerId != 0)
    {
        w.RemoveContactListener(g_entityContactListenerId);
        g_entityContactListenerId = 0;
    }
    if (g_entityTriggerListenerId != 0)
    {
        w.RemoveTriggerListener(g_entityTriggerListenerId);
        g_entityTriggerListenerId = 0;
    }
}

static SubscriptionId AddEntry(Entry e)
{
    if (!e.entity.IsValid())
        return 0;

    e.id = g_nextId++;
    const SubscriptionId id = e.id;
    ListOf(g_byEntity[e.entity], e.kind).push_back(id);
    g_entries.emplace(id, std::move(e));

    EnsureWorldListeners();
    return id;
}

static void RemoveIdFromVector(std::vector<SubscriptionId>& v, SubscriptionId id)
{
    for (size_t i = 0; i < v.size(); ++i)
    {
        if (v[i] == id)
        {
            v[i] = v.back();
            v.pop_back();
            return;
        }
    }
}

// The one place a subscription ends: it leaves the entity's lists, its entry is erased now or, during
// a delivery, when the delivery returns, and its `ended` callback runs.
static void EndSubscription(SubscriptionId id)
{
    const auto it = g_entries.find(id);
    if (it == g_entries.end() || !it->second.live)
        return;

    Entry& e = it->second;
    e.live = false;
    PhysicsEntityEventCallbacks::EndedCallback ended = std::move(e.ended);
    if (auto itL = g_byEntity.find(e.entity); itL != g_byEntity.end())
    {
        auto& lists = itL->second;
        RemoveIdFromVector(ListOf(lists, e.kind), id);
        if (lists.contactBegin.empty() && lists.contactEnd.empty() && lists.triggerEnter.empty() && lists.triggerExit.empty())
            g_byEntity.erase(itL);
    }
    if (g_deliveryDepth == 0)
        g_entries.erase(it);
    else
        g_endedDuringDelivery.push_back(id);

    if (ended)
        ended();
}

static SubscriptionId Subscribe(ECS::EntityHandle entity, EntryKind kind, PhysicsEntityEventCallbacks::ContactCallback contact,
                                PhysicsEntityEventCallbacks::TriggerCallback trigger, PhysicsEntityEventCallbacks::EndedCallback ended)
{
    Entry e{};
    e.entity = entity;
    e.kind = kind;
    e.contactCb = std::move(contact);
    e.triggerCb = std::move(trigger);
    e.ended = std::move(ended);
    return AddEntry(std::move(e));
}
} // namespace

PhysicsEntityEventCallbacks::SubscriptionId PhysicsEntityEventCallbacks::SubscribeContactBegin(ECS::EntityHandle entity,
                                                                                               ContactCallback callback,
                                                                                               EndedCallback ended)
{
    return Subscribe(entity, EntryKind::ContactBegin, std::move(callback), {}, std::move(ended));
}

PhysicsEntityEventCallbacks::SubscriptionId PhysicsEntityEventCallbacks::SubscribeContactEnd(ECS::EntityHandle entity,
                                                                                             ContactCallback callback,
                                                                                             EndedCallback ended)
{
    return Subscribe(entity, EntryKind::ContactEnd, std::move(callback), {}, std::move(ended));
}

PhysicsEntityEventCallbacks::SubscriptionId PhysicsEntityEventCallbacks::SubscribeTriggerEnter(ECS::EntityHandle entity,
                                                                                               TriggerCallback callback,
                                                                                               EndedCallback ended)
{
    return Subscribe(entity, EntryKind::TriggerEnter, {}, std::move(callback), std::move(ended));
}

PhysicsEntityEventCallbacks::SubscriptionId PhysicsEntityEventCallbacks::SubscribeTriggerExit(ECS::EntityHandle entity,
                                                                                              TriggerCallback callback,
                                                                                              EndedCallback ended)
{
    return Subscribe(entity, EntryKind::TriggerExit, {}, std::move(callback), std::move(ended));
}

void PhysicsEntityEventCallbacks::Unsubscribe(SubscriptionId id)
{
    EndSubscription(id);
    MaybeRemoveWorldListeners();
}

void PhysicsEntityEventCallbacks::UnsubscribeAllForEntity(ECS::EntityHandle entity)
{
    auto it = g_byEntity.find(entity);
    if (it == g_byEntity.end())
        return;

    // Copy ids so we can mutate lists while removing.
    std::vector<SubscriptionId> ids;
    ids.insert(ids.end(), it->second.contactBegin.begin(), it->second.contactBegin.end());
    ids.insert(ids.end(), it->second.contactEnd.begin(), it->second.contactEnd.end());
    ids.insert(ids.end(), it->second.triggerEnter.begin(), it->second.triggerEnter.end());
    ids.insert(ids.end(), it->second.triggerExit.begin(), it->second.triggerExit.end());

    for (auto id : ids)
        EndSubscription(id);
    MaybeRemoveWorldListeners();
}

void PhysicsEntityEventCallbacks::ClearAll()
{
    std::vector<SubscriptionId> ids;
    ids.reserve(g_entries.size());
    for (const auto& [id, entry] : g_entries)
        if (entry.live)
            ids.push_back(id);
    for (const SubscriptionId id : ids)
        EndSubscription(id);
    MaybeRemoveWorldListeners();
}

void PhysicsEntityEventCallbacks::NotifyWorldStateChanged()
{
    // If the world just became available, install listeners if we have subscriptions.
    // If it was torn down, ids become invalid and we'll re-install on next init.
    if (!PhysicsWorldService::IsInitialized())
    {
        g_entityContactListenerId = 0;
        g_entityTriggerListenerId = 0;
        return;
    }

    EnsureWorldListeners();
}

} // namespace GameEngine::PhysicsECS
