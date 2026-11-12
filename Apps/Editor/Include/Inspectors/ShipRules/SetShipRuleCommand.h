#pragma once

#include "UndoRedo/IEditorCommand.h"

#include "AssetCore/GUID.h"
#include "Engine/Build/ShipRules.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine::Editor
{

// One undoable change of the own ship rule of the selected files and folders. A file's rule is
// the asset database key ShipRules::kAssetMetaKey, written through the asset registry; a folder's
// rule is its .assetfolder file. Undo puts back each previous rule, or its absence.
class SetShipRuleCommand final : public IEditorCommand
{
  public:
    // `rule` empty clears the own rules. `onWritten` re-presents the Inspector after Do and Undo.
    SetShipRuleCommand(const std::vector<std::filesystem::path>& paths, std::optional<ShipRule> rule,
                       std::function<void()> onWritten);

    const char* GetName() const override { return "Change Ship Rule"; }
    const char* GetTypeName() const override { return "SetShipRuleCommand"; }

    void Do() override;
    void Undo() override;

  private:
    // A file is held by GUID, so the command follows a rename; a folder is held by path.
    struct Target
    {
        GUID Asset;
        std::filesystem::path Folder;
        std::optional<std::string> Previous; // The stored text before Do; empty when there was none.
    };

    // Writes `text` as the target's stored rule text, or removes the rule when `text` is empty.
    // A write that fails logs an error.
    static void Write(const Target& target, const std::optional<std::string>& text);
    static std::optional<std::string> Read(const Target& target);

    std::vector<Target> m_Targets;
    std::optional<ShipRule> m_Rule;
    bool m_PreviousCaptured = false;
    std::function<void()> m_OnWritten;
};

} // namespace GameEngine::Editor
