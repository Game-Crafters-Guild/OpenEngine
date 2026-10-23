#include "DayCycle/DayCycleWorkingCopy.h"

#include "AssetCore/SharedFileRead.h"
#include "Assets/AssetManager.h"
#include "UndoRedo/IEditorCommand.h"
#include "UndoRedo/UndoRedoService.h"

#include <algorithm>

namespace GameEngine::Editor
{
namespace
{
struct WorkingCopies
{
    std::vector<std::shared_ptr<DayCycleWorkingCopy>> Open;
    std::weak_ptr<DayCycleWorkingCopy> MostRecent;
    std::uint64_t MostRecentGeneration = 0;
};

WorkingCopies& Copies()
{
    static WorkingCopies copies;
    return copies;
}

// One committed edit of a working copy: the profile before and after it.
class DayCycleProfileEditCommand final : public IEditorCommand
{
  public:
    DayCycleProfileEditCommand(std::weak_ptr<DayCycleWorkingCopy> copy, std::string label, DayCycleProfileData before,
                               DayCycleProfileData after)
        : m_Copy(std::move(copy)), m_Label(std::move(label)), m_Before(std::move(before)), m_After(std::move(after))
    {
    }

    const char* GetName() const override { return m_Label.c_str(); }
    const char* GetTypeName() const override { return "DayCycleProfileEditCommand"; }
    void Do() override { Apply(m_After); }
    void Undo() override { Apply(m_Before); }

  private:
    void Apply(const DayCycleProfileData& data)
    {
        if (const std::shared_ptr<DayCycleWorkingCopy> copy = m_Copy.lock())
            copy->Restore(data);
    }

    std::weak_ptr<DayCycleWorkingCopy> m_Copy;
    std::string m_Label;
    DayCycleProfileData m_Before;
    DayCycleProfileData m_After;
};

bool ReadProfile(AssetManager& assets, const GUID& guid, const std::filesystem::path& path, DayCycleProfileData& out,
                 std::string& outError)
{
    if (const auto asset = std::dynamic_pointer_cast<DayCycleProfile>(assets.GetAsset(guid)))
    {
        if (const std::shared_ptr<const DayCycleProfileData> data = asset->GetData())
        {
            out = *data;
            return true;
        }
    }
    std::string text;
    if (!ReadFileTextShared(path, text))
    {
        outError = "Could not read " + path.string() + ".";
        return false;
    }
    return ParseDayCycleProfile(text, out, outError);
}
} // namespace

std::shared_ptr<DayCycleWorkingCopy> DayCycleWorkingCopy::Open(AssetManager& assets,
                                                               const std::filesystem::path& profilePath,
                                                               std::string& outError)
{
    // One file has one GUID, however its path is spelled: the registry hands out its stored path
    // (case and separators folded), the window opens the Default from the editor source's root, and
    // Save a copy opens the path it wrote. A path the registry does not know matches by path alone.
    WorkingCopies& copies = Copies();
    const GUID guid = assets.ResolveAssetGuid(profilePath);
    for (const std::shared_ptr<DayCycleWorkingCopy>& copy : copies.Open)
    {
        if (guid.IsNull() ? copy->GetPath() == profilePath : copy->GetGuid() == guid)
        {
            Show(copy);
            return copy;
        }
    }
    DayCycleProfileData data;
    if (!ReadProfile(assets, guid, profilePath, data, outError))
        return nullptr;
    auto copy = std::make_shared<DayCycleWorkingCopy>(assets, guid, profilePath, std::move(data));
    copies.Open.push_back(copy);
    Show(copy);
    return copy;
}

std::shared_ptr<DayCycleWorkingCopy> DayCycleWorkingCopy::MostRecent()
{
    return Copies().MostRecent.lock();
}

void DayCycleWorkingCopy::Show(const std::shared_ptr<DayCycleWorkingCopy>& copy)
{
    WorkingCopies& copies = Copies();
    if (copies.MostRecent.lock() == copy)
        return;
    copies.MostRecent = copy;
    ++copies.MostRecentGeneration;
}

std::uint64_t DayCycleWorkingCopy::MostRecentGeneration()
{
    return Copies().MostRecentGeneration;
}

void DayCycleWorkingCopy::CloseAll()
{
    WorkingCopies& copies = Copies();
    copies.Open.clear();
    copies.MostRecent.reset();
    ++copies.MostRecentGeneration;
}

EditorChangeNotifications::SubscriptionToken
DayCycleWorkingCopy::CloseAllOnProjectSwitch(EditorChangeNotifications& notifications)
{
    return notifications.SubscribeProjectSwitched([]() { CloseAll(); });
}

DayCycleWorkingCopy::DayCycleWorkingCopy(AssetManager& assets, const GUID& guid, std::filesystem::path path,
                                         DayCycleProfileData data)
    : m_Assets(assets), m_Guid(guid), m_Path(std::move(path)), m_Data(std::move(data)), m_Committed(m_Data),
      m_Saved(m_Data), m_SavedText(WriteDayCycleProfile(m_Data))
{
}

bool DayCycleWorkingCopy::IsDirty() const
{
    return WriteDayCycleProfile(m_Data) != m_SavedText;
}

DayCycleSaveAction DayCycleWorkingCopy::GetSaveAction() const
{
    return DayCycleSaveActionFor(m_Assets, m_Path);
}

void DayCycleWorkingCopy::Preview(const DayCycleProfileData& data)
{
    SetData(data);
}

void DayCycleWorkingCopy::Commit(const DayCycleProfileData& data, const std::string& label, UndoRedoService* undo)
{
    DayCycleProfileData before = m_Committed;
    if (!undo)
    {
        Restore(data);
        return;
    }
    undo->Execute(std::make_unique<DayCycleProfileEditCommand>(weak_from_this(), label, std::move(before), data));
}

void DayCycleWorkingCopy::Restore(const DayCycleProfileData& data)
{
    m_Committed = data;
    SetData(data);
}

bool DayCycleWorkingCopy::Save(std::string& outError)
{
    if (!SaveDayCycleProfile(m_Assets, m_Path, m_Data, outError))
        return false;
    m_Saved = m_Data;
    m_SavedText = WriteDayCycleProfile(m_Data);
    NotifyChanged();
    return true;
}

std::shared_ptr<DayCycleWorkingCopy> DayCycleWorkingCopy::SaveCopy(std::string& outError)
{
    const std::filesystem::path path = SaveDayCycleProfileCopy(m_Assets, m_Data, outError);
    if (path.empty())
        return nullptr;
    std::shared_ptr<DayCycleWorkingCopy> copy = Open(m_Assets, path, outError);
    if (copy)
        Restore(m_Saved);
    return copy;
}

std::uint64_t DayCycleWorkingCopy::Subscribe(std::function<void()> callback)
{
    const std::uint64_t token = m_NextToken++;
    m_Subscribers.emplace_back(token, std::move(callback));
    return token;
}

void DayCycleWorkingCopy::Unsubscribe(std::uint64_t token)
{
    std::erase_if(m_Subscribers, [token](const auto& subscriber) { return subscriber.first == token; });
}

void DayCycleWorkingCopy::SetData(const DayCycleProfileData& data)
{
    m_Data = data;
    NotifyChanged();
}

void DayCycleWorkingCopy::NotifyChanged()
{
    // A callback may rebuild its surface and unsubscribe; run a snapshot.
    const auto subscribers = m_Subscribers;
    for (const auto& [token, callback] : subscribers)
    {
        const bool stillSubscribed = std::any_of(m_Subscribers.begin(), m_Subscribers.end(),
                                                 [token](const auto& subscriber) { return subscriber.first == token; });
        if (stillSubscribed && callback)
            callback();
    }
}

} // namespace GameEngine::Editor
