#pragma once

#include "UndoRedo/IEditorCommand.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace GameEngine::Editor
{

/// Branch-preserving revisions. The root is the state before its children.
class HistoryGraph
{
public:
    using NodeId = std::uint64_t;

    struct Node
    {
        NodeId Id = 0;
        NodeId Parent = 0;
        NodeId PreferredChild = 0;
        std::chrono::system_clock::time_point Timestamp;
        const char* GetName() const { return Command->GetName(); }
        const char* GetTypeName() const { return Command->GetTypeName(); }

    private:
        friend class HistoryGraph;
        std::unique_ptr<IEditorCommand> Command;
    };

    NodeId GetCurrent() const { return m_Current; }
    NodeId GetRoot() const { return m_Root; }
    const std::vector<Node>& GetNodes() const { return m_Nodes; }

    const Node* Find(NodeId id) const
    {
        const auto found = std::lower_bound(m_Nodes.begin(), m_Nodes.end(), id,
            [](const Node& node, NodeId value) { return node.Id < value; });
        return found != m_Nodes.end() && found->Id == id ? &*found : nullptr;
    }

    std::vector<NodeId> GetAncestors() const
    {
        std::vector<NodeId> result;
        for (NodeId id = m_Current; id != m_Root;)
        {
            const Node* node = Find(id);
            if (!node)
                break;
            result.push_back(id);
            id = node->Parent;
        }
        std::reverse(result.begin(), result.end());
        return result;
    }

    std::vector<NodeId> GetFuture() const
    {
        std::vector<NodeId> result;
        NodeId id = PreferredChild(m_Current);
        while (const Node* node = Find(id))
        {
            result.push_back(id);
            id = node->PreferredChild;
        }
        return result;
    }

    void Append(NodeId id, std::unique_ptr<IEditorCommand> command, std::size_t limit)
    {
        SetPreferredChild(m_Current, id);
        Node node;
        node.Id = id;
        node.Parent = m_Current;
        node.Timestamp = std::chrono::system_clock::now();
        node.Command = std::move(command);
        m_Nodes.push_back(std::move(node));
        m_Current = id;
        Trim(limit);
    }

    bool Undo()
    {
        const Node* node = Find(m_Current);
        return node && Checkout(node->Parent);
    }

    bool Redo()
    {
        const NodeId child = PreferredChild(m_Current);
        return child != 0 && Checkout(child);
    }

    /// Replay through the common ancestor. Failed traversal restores its starting revision.
    bool Checkout(NodeId destination)
    {
        if (destination == m_Current)
            return true;
        if (destination != m_Root && !Find(destination))
            return false;

        std::vector<NodeId> backwards;
        std::vector<NodeId> forwards;
        NodeId left = m_Current;
        NodeId right = destination;
        // Parents precede their children because node IDs are never reused.
        while (left != right)
        {
            if (left > right)
            {
                backwards.push_back(left);
                left = Find(left)->Parent;
            }
            else
            {
                forwards.push_back(right);
                right = Find(right)->Parent;
            }
        }

        std::size_t undone = 0;
        for (NodeId id : backwards)
        {
            if (!Find(id)->Command->TryUndo())
            {
                RestoreUndone(backwards, undone);
                return false;
            }
            ++undone;
        }

        std::reverse(forwards.begin(), forwards.end());
        std::size_t redone = 0;
        for (NodeId id : forwards)
        {
            if (!Find(id)->Command->TryRedo())
            {
                while (redone > 0)
                    Find(forwards[--redone])->Command->TryUndo();
                RestoreUndone(backwards, undone);
                return false;
            }
            ++redone;
        }

        for (NodeId id : backwards)
            SetPreferredChild(Find(id)->Parent, id);
        for (NodeId id : forwards)
            SetPreferredChild(Find(id)->Parent, id);
        m_Current = destination;
        for (NodeId id : backwards)
            Find(id)->Command->NotifyReplay();
        for (NodeId id : forwards)
            Find(id)->Command->NotifyReplay();
        return true;
    }

    std::vector<std::unique_ptr<IEditorCommand>> TakeActiveCommands()
    {
        std::vector<std::unique_ptr<IEditorCommand>> commands;
        for (NodeId id : GetAncestors())
            commands.push_back(std::move(FindMutable(id)->Command));
        *this = {};
        return commands;
    }

private:
    Node* FindMutable(NodeId id) { return const_cast<Node*>(Find(id)); }

    NodeId PreferredChild(NodeId id) const
    {
        const Node* node = Find(id);
        return node ? node->PreferredChild : m_RootPreferredChild;
    }

    void SetPreferredChild(NodeId id, NodeId child)
    {
        if (Node* node = FindMutable(id))
            node->PreferredChild = child;
        else
            m_RootPreferredChild = child;
    }

    void RestoreUndone(const std::vector<NodeId>& backwards, std::size_t count)
    {
        while (count > 0)
            Find(backwards[--count])->Command->TryRedo();
    }

    void Trim(std::size_t limit)
    {
        while (limit != 0 && m_Nodes.size() > limit)
        {
            const auto path = GetAncestors();
            const auto leaf = std::find_if(m_Nodes.begin(), m_Nodes.end(), [&](const Node& node)
            {
                return std::find(path.begin(), path.end(), node.Id) == path.end() &&
                    std::none_of(m_Nodes.begin(), m_Nodes.end(), [&](const Node& child) { return child.Parent == node.Id; });
            });
            if (leaf != m_Nodes.end())
            {
                if (PreferredChild(leaf->Parent) == leaf->Id)
                    SetPreferredChild(leaf->Parent, 0);
                m_Nodes.erase(leaf);
                continue;
            }
            const NodeId newRoot = path.front();
            m_RootPreferredChild = PreferredChild(newRoot);
            m_Root = newRoot;
            std::erase_if(m_Nodes, [newRoot](const Node& node) { return node.Id == newRoot; });
        }
    }

    std::vector<Node> m_Nodes;
    NodeId m_Current = 0;
    NodeId m_Root = 0;
    NodeId m_RootPreferredChild = 0;
};

} // namespace GameEngine::Editor
