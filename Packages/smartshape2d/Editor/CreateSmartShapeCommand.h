#pragma once

#include "Editor/Registries/EditorPluginRegistry.h"
#include "UndoRedo/IEditorCommand.h"

namespace GameEngine::SmartShape2D
{
enum class ShapeCreationKind { Closed, Open, EdgeOverride };

class CreateSmartShapeCommand final : public Editor::IEditorCommand
{
public:
    CreateSmartShapeCommand(const Editor::HierarchyCommandContext& context, ShapeCreationKind kind);
    const char* GetName() const override { return "Create Smart Shape 2D"; }
    void Do() override;
    void Undo() override;

private:
    Editor::HierarchyCommandContext m_Context;
    ECS::EntityHandle m_Entity;
    ShapeCreationKind m_Kind = ShapeCreationKind::Closed;
    void NotifyCreated();
    bool m_Undone = false;
};
} // namespace GameEngine::SmartShape2D
