#pragma once

#include "Panels/GraphPanel.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine
{
class GraphPanel::GraphSnapshotCommand final : public Editor::IEditorCommand
{
public:
    GraphSnapshotCommand(GraphPanel* panel,
                         std::vector<std::uint8_t> before,
                         std::vector<std::uint8_t> after,
                         std::string name)
        : m_Panel(UIElement::MakeWeakRef(panel)), m_DocumentId(panel->m_DocumentId),
          m_Before(std::move(before)), m_After(std::move(after)), m_Name(std::move(name))
    {
    }

    const char* GetName() const override { return m_Name.c_str(); }
    void Do() override { TryRedo(); }
    void Undo() override { TryUndo(); }
    void Redo() override { TryRedo(); }
    bool TryUndo() override { return Apply(m_Before); }
    bool TryRedo() override { return Apply(m_After); }

private:
    bool Apply(const std::vector<std::uint8_t>& bytes)
    {
        auto* panel = m_Panel.Get();
        return panel && panel->m_DocumentId == m_DocumentId && panel->ApplyGraphSnapshot(bytes);
    }

    UIElement::WeakRef<GraphPanel> m_Panel;
    std::uint64_t m_DocumentId;
    std::vector<std::uint8_t> m_Before;
    std::vector<std::uint8_t> m_After;
    std::string m_Name;
};
} // namespace GameEngine
