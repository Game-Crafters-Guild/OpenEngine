#pragma once

#include "AssetCore/GUID.h"
#include "Assets/DayCycleProfile.h"
#include "DayCycle/DayCycleProfileSave.h"
#include "EditorChangeNotifications.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine
{
class AssetManager;
}

namespace GameEngine::Editor
{
class UndoRedoService;

/// The one editable copy of a Day Cycle profile that every editing surface shares: the Day Cycle
/// window and the profile's inspector edit the same object, so they have one undo history (the
/// editor's), one unsaved mark and one Save. A copy lives for the editor session, so an unsaved
/// edit survives closing both surfaces. Main thread only.
class DayCycleWorkingCopy : public std::enable_shared_from_this<DayCycleWorkingCopy>
{
  public:
    /// The working copy of the profile at `profilePath`, opened from the loaded asset (or the file
    /// when it is not loaded) the first time it is asked for. Copies are one per asset GUID, so two
    /// spellings of one file's path give one copy. Null, with the reason in `outError`,
    /// when the file does not parse.
    static std::shared_ptr<DayCycleWorkingCopy> Open(AssetManager& assets, const std::filesystem::path& profilePath,
                                                     std::string& outError);
    /// The copy the last Open or Show named: the profile the Day Cycle window shows. Null before any.
    static std::shared_ptr<DayCycleWorkingCopy> MostRecent();
    /// Makes `copy` the most recent, as an Open of it does.
    static void Show(const std::shared_ptr<DayCycleWorkingCopy>& copy);
    /// A number that changes whenever the most recent copy changes.
    static std::uint64_t MostRecentGeneration();
    /// Drops every copy: a project switch, and each test's start and end.
    static void CloseAll();
    /// Closes every copy whenever `notifications` reports a project switch, so no surface shows or
    /// saves the previous project's profile. The subscription lasts as long as `notifications`.
    static EditorChangeNotifications::SubscriptionToken CloseAllOnProjectSwitch(EditorChangeNotifications& notifications);

    DayCycleWorkingCopy(AssetManager& assets, const GUID& guid, std::filesystem::path path, DayCycleProfileData data);

    const GUID& GetGuid() const { return m_Guid; }
    const std::filesystem::path& GetPath() const { return m_Path; }
    const DayCycleProfileData& GetData() const { return m_Data; }
    /// The profile differs from what its file holds.
    bool IsDirty() const;
    /// Save in place for a project profile; Save a copy to the project for any other.
    DayCycleSaveAction GetSaveAction() const;

    /// Shows `data` on every surface without an undo step: a drag in progress.
    void Preview(const DayCycleProfileData& data);
    /// Makes `data` the profile as one undo step named `label`, taken from the last committed
    /// profile, so a drag's previews and its end are one step. Without an undo service the edit
    /// still applies.
    void Commit(const DayCycleProfileData& data, const std::string& label, UndoRedoService* undo);
    /// Puts back a committed state: the undo history's own write.
    void Restore(const DayCycleProfileData& data);

    /// Writes the profile into its file. Refused for a profile that is not the project's.
    bool Save(std::string& outError);
    /// Writes the profile as a new project profile, opens its working copy and makes it the most
    /// recent; this copy goes back to its file's profile, since its edits now live in the new one. The
    /// new copy, or null with the reason in `outError`.
    std::shared_ptr<DayCycleWorkingCopy> SaveCopy(std::string& outError);

    /// Called after every change of the profile or its unsaved mark. The token unsubscribes.
    std::uint64_t Subscribe(std::function<void()> callback);
    void Unsubscribe(std::uint64_t token);

  private:
    void SetData(const DayCycleProfileData& data);
    void NotifyChanged();

    AssetManager& m_Assets;
    GUID m_Guid;
    std::filesystem::path m_Path;
    DayCycleProfileData m_Data;
    // The profile as the undo history knows it: the last commit or restore.
    DayCycleProfileData m_Committed;
    // The file's profile and its text at open or the last save.
    DayCycleProfileData m_Saved;
    std::string m_SavedText;
    std::uint64_t m_NextToken = 1;
    std::vector<std::pair<std::uint64_t, std::function<void()>>> m_Subscribers;
};

} // namespace GameEngine::Editor
