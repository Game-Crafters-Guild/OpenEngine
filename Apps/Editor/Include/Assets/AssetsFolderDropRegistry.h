#pragma once

#include "Types/FlatMap.h"
#include "UI/Interaction/Types.h"

#include <filesystem>
#include <vector>

namespace GameEngine
{
struct EditorContext;
} // namespace GameEngine

namespace GameEngine::UI::Interaction
{
struct DropRequest;
}

namespace GameEngine::Editor
{

/// A drop on an Assets folder of a drag that does not carry asset files: the drop, the folder it
/// lands in and the services of the Assets panel.
struct AssetsFolderDrop
{
    const UI::Interaction::DropRequest& Request;
    /// The absolute path of the folder under the pointer.
    std::filesystem::path Folder;
    const EditorContext* EditorCtx = nullptr;
};

/// What an editor feature does with a drag of one payload type dropped on an Assets folder.
struct AssetsFolderDropAcceptor
{
    /// The highlight decision while the drag hovers the folder. A refusal gives the reason in plain words.
    UI::Interaction::DropFeedback (*CanDrop)(const AssetsFolderDrop& drop) = nullptr;
    /// Runs on the release after CanDrop allowed the drop. Returns the files it wrote; the Assets panel
    /// lists the drop folder and selects them.
    std::vector<std::filesystem::path> (*PerformDrop)(const AssetsFolderDrop& drop) = nullptr;
};

/// The drops the Assets views accept on a folder besides asset files, keyed by payload type. The
/// feature that owns a payload registers its acceptor at startup; the Assets panel consults the
/// registry and does not name the feature.
class AssetsFolderDropRegistry
{
  public:
    static AssetsFolderDropRegistry& Get();

    /// Sets the acceptor of `payloadType`. A second registration of the type replaces the first.
    void Register(UI::Interaction::PayloadTypeId payloadType, AssetsFolderDropAcceptor acceptor);

    bool Accepts(UI::Interaction::PayloadTypeId payloadType) const;
    /// Refused when no acceptor is registered for the drop's payload type.
    UI::Interaction::DropFeedback CanDrop(const AssetsFolderDrop& drop) const;
    /// The files the acceptor wrote; empty when no acceptor is registered for the drop's payload type.
    std::vector<std::filesystem::path> PerformDrop(const AssetsFolderDrop& drop) const;

  private:
    FlatMap<UI::Interaction::PayloadTypeId, AssetsFolderDropAcceptor> m_Acceptors;
};

} // namespace GameEngine::Editor
