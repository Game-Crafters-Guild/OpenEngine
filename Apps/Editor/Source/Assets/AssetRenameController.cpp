#include "Assets/AssetRenameController.h"

#include "Assets/AssetPathKey.h"
#include "Assets/AssetRename.h"
#include "UndoRedo/RenameAssetCommand.h"
#include "UndoRedo/UndoRedoService.h"

#include <memory>
#include <utility>

namespace GameEngine {

void AssetRenameController::Begin(Label* title,
                                  UIElement* focusOnEnd,
                                  const std::filesystem::path& itemPath,
                                  const char* fieldClass,
                                  Editor::UndoRedoService* undo,
                                  AssetManager* assets,
                                  OnRenamed onRenamed)
{
    if (m_Session.IsActive() && m_ItemPath == itemPath)
        return;
    if (itemPath.empty())
        return;
    m_ItemPath = itemPath;

    auto validate = [itemPath](const std::string& name)
    {
        const Editor::AssetRenameValidation validation = Editor::ValidateAssetRename(name, itemPath);
        return validation.Ok ? std::string{} : validation.Reason;
    };
    auto commit = [itemPath, undo, assets, onRenamed = std::move(onRenamed)](const std::string& name)
    {
        auto command = std::make_unique<Editor::RenameAssetCommand>(Editor::PlanAssetRename(itemPath, name),
                                                                         assets, onRenamed);
        if (undo)
            undo->Execute(std::move(command));
        else
            command->Do();
    };
    m_Session.Begin(title, focusOnEnd, Editor::AssetRenameEditableName(itemPath), fieldClass, std::move(validate),
                    std::move(commit));
}

void AssetRenameController::OnRowBound(const UIElement& row, const std::filesystem::path& shownPath)
{
    if (!m_Session.IsActive())
        return;
    m_Session.OnRowBound(row, AssetPathKey(shownPath) == AssetPathKey(m_ItemPath));
}

} // namespace GameEngine
