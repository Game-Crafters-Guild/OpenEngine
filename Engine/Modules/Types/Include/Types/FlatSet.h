#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <functional>
#include <vector>

namespace GameEngine
{

// Sorted std::vector<K> with binary search: FlatMap's contract without the values. Optimal for
// <50 entries where cache locality matters more than asymptotic complexity. Iteration visits the
// keys in order and is read-only, so the order cannot be broken. Insert and Erase invalidate every
// iterator, as std::vector's insert and erase do: never change the set while walking it.
template <typename K, typename Compare = std::less<K>>
    requires std::strict_weak_order<Compare, const K&, const K&>
class FlatSet
{
  public:
    using value_type = K;
    using const_iterator = typename std::vector<K>::const_iterator;

    // Adds `key` if it is missing. Returns true when it was added.
    bool Insert(const K& key)
    {
        auto it = LowerBound(key);
        if (it != m_Data.end() && !Compare{}(key, *it))
            return false;
        m_Data.insert(it, key);
        return true;
    }

    bool Erase(const K& key)
    {
        auto it = LowerBound(key);
        if (it != m_Data.end() && !Compare{}(key, *it))
        {
            m_Data.erase(it);
            return true;
        }
        return false;
    }

    bool Contains(const K& key) const
    {
        auto it = LowerBound(key);
        return it != m_Data.end() && !Compare{}(key, *it);
    }

    size_t Size() const { return m_Data.size(); }
    bool Empty() const { return m_Data.empty(); }
    void Clear() { m_Data.clear(); }
    void Reserve(size_t n) { m_Data.reserve(n); }

    const_iterator begin() const { return m_Data.begin(); }
    const_iterator end() const { return m_Data.end(); }

  private:
    std::vector<K> m_Data;

    typename std::vector<K>::iterator LowerBound(const K& key)
    {
        return std::lower_bound(m_Data.begin(), m_Data.end(), key, Compare{});
    }

    const_iterator LowerBound(const K& key) const
    {
        return std::lower_bound(m_Data.begin(), m_Data.end(), key, Compare{});
    }
};

} // namespace GameEngine
