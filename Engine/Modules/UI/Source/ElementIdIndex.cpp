#include "UI/Internal/ElementIdIndex.h"

#include "Logger/Logger.h"
#include "UI/Internal/AttachedRootAccess.h"
#include "UI/UIElement.h"

#include <algorithm>
#include <cassert>

namespace GameEngine::UI
{
namespace
{

size_t LookupDepth(const UIElement& element)
{
    size_t depth = 0;
    for (const UIElement* parent = element.GetDfsParent(); parent; parent = parent->GetDfsParent())
        ++depth;
    return depth;
}

bool PrecedesInLookupOrder(const UIElement& first, const UIElement& second)
{
    const size_t firstDepth = LookupDepth(first);
    const size_t secondDepth = LookupDepth(second);
    const UIElement* firstBranch = &first;
    const UIElement* secondBranch = &second;
    for (size_t depth = firstDepth; depth > secondDepth; --depth)
        firstBranch = firstBranch->GetDfsParent();
    for (size_t depth = secondDepth; depth > firstDepth; --depth)
        secondBranch = secondBranch->GetDfsParent();
    if (firstBranch == secondBranch)
        return firstDepth < secondDepth;

    // Both matches are reachable from the requested subtree, so their chains
    // share an ancestor. Compare the branches at their first divergence.
    while (firstBranch->GetDfsParent() != secondBranch->GetDfsParent())
    {
        firstBranch = firstBranch->GetDfsParent();
        secondBranch = secondBranch->GetDfsParent();
    }
    for (const auto& child : firstBranch->GetDfsParent()->GetChildren())
    {
        if (child.get() == firstBranch)
            return true;
        if (child.get() == secondBranch)
            return false;
    }
    // A Mount target follows every ordinary child in lookup order.
    return false;
}

bool IsInSubtree(const UIElement& element, const UIElement& subtree)
{
    for (const UIElement* ancestor = &element; ancestor; ancestor = ancestor->GetDfsParent())
    {
        if (ancestor == &subtree)
            return true;
    }
    return false;
}

void ReportIdCollision(const UIElement& registered, const UIElement& refused)
{
    Logger::Log::Error("UI ids '{}' and '{}' have the same StringId; '{}' is not findable by id. Rename one of them.",
                       registered.GetId(), refused.GetId(), refused.GetId());
    Logger::Log::Flush();
    assert(false && "two UI ids have the same StringId; rename one of them");
}

} // namespace

void ElementIdIndex::Register(UIElement& element)
{
    if (element.GetId().empty())
        return;
    auto& matches = m_Elements[element.GetIdHash()];
    // Every registered candidate carries the same id string, so the first one stands
    // for all of them: the same string is a duplicate id (allowed), a different one a
    // collision.
    if (!matches.empty() && matches.front()->GetId() != element.GetId())
    {
        ReportIdCollision(*matches.front(), element);
        return;
    }
    if (std::find(matches.begin(), matches.end(), &element) == matches.end())
        matches.push_back(&element);
}

void ElementIdIndex::Unregister(UIElement& element)
{
    if (element.GetId().empty())
        return;
    auto entry = m_Elements.find(element.GetIdHash());
    if (entry == m_Elements.end())
        return;
    std::erase(entry->second, &element);
    if (entry->second.empty())
        m_Elements.erase(entry);
}

UIElement* ElementIdIndex::Find(const UIElement& subtree, StringId id) const
{
    if (id == 0)
        return nullptr;
    const auto entry = m_Elements.find(id);
    if (entry == m_Elements.end())
        return nullptr;
    // From a manager root, membership is the element's attached root: every element under
    // that root names it, and nothing else does. Anywhere else the parent chain decides.
    const bool fromManagerRoot = UIAttachedRootAccess::AttachedRoot(subtree) == &subtree;
    UIElement* firstMatch = nullptr;
    for (UIElement* element : entry->second)
    {
        // Ownership outlives a dock tab's mount, so an owned element may sit outside every
        // manager root (an inactive tab's panel) and is skipped here.
        const bool inSubtree = fromManagerRoot ? UIAttachedRootAccess::AttachedRoot(*element) == &subtree
                                               : IsInSubtree(*element, subtree);
        if (!inSubtree)
            continue;
        if (element == &subtree)
            return element;
        if (!firstMatch || PrecedesInLookupOrder(*element, *firstMatch))
            firstMatch = element;
    }
    return firstMatch;
}

} // namespace GameEngine::UI
