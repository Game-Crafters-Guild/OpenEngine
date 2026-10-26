#pragma once

#include "Types/Types.h"

#include <cstddef>
#include <numeric>
#include <vector>

namespace GameEngine
{

// Disjoint sets over the items 0 to count - 1 (union-find): each item starts in a set of its own,
// Join merges two sets and Find names an item's set by one of its items, the same for every item
// of the set until the next Join. Find halves the path it walks, so a chain of joins stays cheap.
class DisjointSets
{
  public:
    explicit DisjointSets(std::size_t count) : m_Parent(count) { std::iota(m_Parent.begin(), m_Parent.end(), 0u); }

    uint32 Find(uint32 item)
    {
        while (m_Parent[item] != item)
        {
            m_Parent[item] = m_Parent[m_Parent[item]];
            item = m_Parent[item];
        }
        return item;
    }

    void Join(uint32 first, uint32 second) { m_Parent[Find(first)] = Find(second); }

  private:
    std::vector<uint32> m_Parent;
};

} // namespace GameEngine
