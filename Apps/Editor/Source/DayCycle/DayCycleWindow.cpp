#include "DayCycle/DayCycleWindow.h"

#include "Assets/AssetRegistry.h"
#include "Assets/DayCycleProfile.h"
#include "Components/Name.h"
#include "Components/Rendering/PostProcessVolume.h"
#include "DayCycle/DayCycleAxis.h"
#include "DayCycle/DayCycleProfileControls.h"
#include "DayCycle/DayCycleScrub.h"
#include "DayCycle/DayCycleShippedProfiles.h"
#include "DayCycle/DayCycleSunNow.h"
#include "DayCycle/DayCycleTrackRows.h"
#include "DayCycle/DayCycleUi.h"
#include "DayCycle/DayCycleWorkingCopy.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Inspectors/SkySun/SkySunReadoutText.h"
#include "Types/StringId.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/CurveField.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <utility>

namespace GameEngine::Editor
{
namespace
{
constexpr const char* kHiddenClass = "hidden";

template <typename T>
UIElement::WeakRef<T> FindIn(UIElement& root, StringId id)
{
    return UIElement::MakeWeakRef(dynamic_cast<T*>(root.FindById(id)));
}

void ShowWhen(UIElement* element, bool shown)
{
    if (!element)
        return;
    if (shown)
        element->RemoveClass(kHiddenClass);
    else
        element->AddClass(kHiddenClass);
}
} // namespace

DayCycleWindow::DayCycleWindow(DayCycleEditorServices services)
    : AssetBoundDockPanel("Day Cycle", std::string(kAssetSourceAliasEditor), "UI/panels/DayCycleWindow.uxml", {}),
      m_Services(std::move(services))
{
    AddClass("day-cycle-window");
    RequestSubtreeStyleAssetPath("UI/panels/DayCycle.css");
    if (m_Services.Assets)
        m_Below = ReadDefaultDayCycleProfile(*m_Services.Assets);
    OpenScrub();
    Refresh();
}

DayCycleWindow::~DayCycleWindow()
{
    m_Rows.reset();
    if (UIManager* manager = m_RefreshManager.Get(); manager && m_RefreshToken != 0)
        manager->UnregisterPeriodicRefresh(m_RefreshToken);
}

void DayCycleWindow::OnOwnerManagerChanged(UIManager* owner)
{
    AssetBoundDockPanel::OnOwnerManagerChanged(owner);
    if (UIManager* previous = m_RefreshManager.Get(); previous && m_RefreshToken != 0)
        previous->UnregisterPeriodicRefresh(m_RefreshToken);
    m_RefreshToken = 0;
    m_RefreshManager = UIManagerRef(owner);
    if (!owner)
    {
        // Closed: the scene renders its own hour again.
        m_Scrub.reset();
        return;
    }
    OpenScrub();
    m_RefreshToken = owner->RegisterPeriodicRefresh([this]() { Refresh(); });
}

void DayCycleWindow::OnLayoutBound()
{
    BindElements();
}

void DayCycleWindow::OnLayoutReconciled()
{
    BindElements();
}

void DayCycleWindow::BindElements()
{
    m_ProfileName = FindIn<Label>(*this, "DayCycleProfileName"_sid);
    m_VolumeLabel = FindIn<Label>(*this, "DayCycleVolume"_sid);
    m_AxisHost = FindIn<UIElement>(*this, "DayCycleAxisHost"_sid);
    m_SaveHost = FindIn<UIElement>(*this, "DayCycleSaveHost"_sid);
    m_PresetsButton = FindIn<Button>(*this, "DayCyclePresetsButton"_sid);
    m_PresetsHost = FindIn<UIElement>(*this, "DayCyclePresetsHost"_sid);
    m_ScrubReadout = FindIn<Label>(*this, "DayCycleScrubReadout"_sid);
    if (Label* scrubLabel = dynamic_cast<Label*>(FindById("DayCycleScrubLabel"_sid)))
        scrubLabel->SetTooltip("Drag the playhead on the strip or on a track to preview the hour. While the playhead is "
                               "held, Animate Time of Day pauses. Closing the window returns the scene's own time; "
                               "saving the scene keeps its own time.");
    m_Notice = FindIn<Label>(*this, "DayCycleNotice"_sid);
    m_DuskCaption = FindIn<Label>(*this, "DayCycleDuskCaption"_sid);
    m_TracksScroll = FindIn<ScrollView>(*this, "DayCycleTracksScroll"_sid);
    if (Button* presets = m_PresetsButton.Get())
        presets->SetOnClick([this](UIEvent&) {
            m_ShowPresets = !m_ShowPresets;
            RequestBuild();
        });
    m_TracksContent = {};
    m_ShownVolume = ECS::EntityHandle{};
    m_NeedsBuild = true;
    Refresh();
}

void DayCycleWindow::OpenScrub()
{
    if (m_Scrub)
        return;
    ECS::World* world = m_Services.World ? m_Services.World() : nullptr;
    Engine::Renderer::SkyEnvironmentSystem* sky = m_Services.Sky ? m_Services.Sky() : nullptr;
    if (world && sky)
        m_Scrub = std::make_unique<DayCycleScrub>(*world, *sky, m_Services.Undo, m_Services.Notifications,
                                                  m_Services.IsPlaying);
}

ECS::EntityHandle DayCycleWindow::ContextVolume() const
{
    ECS::World* world = m_Services.World ? m_Services.World() : nullptr;
    if (!world)
        return {};
    if (m_Services.Selection)
    {
        for (const ECS::EntityHandle entity : m_Services.Selection())
        {
            if (world->IsValid(entity) && world->HasComponent<Components::PostProcessVolume>(entity))
                return entity;
        }
    }
    ECS::EntityHandle global{};
    world->Query<ECS::Read<Components::PostProcessVolume>>().Each(
        [&global](ECS::EntityHandle entity, const Components::PostProcessVolume& volume) {
            if (!global.IsValid() && volume.IsGlobal)
                global = entity;
        });
    return global;
}

std::string DayCycleWindow::VolumeName(ECS::EntityHandle volume) const
{
    ECS::World* world = m_Services.World ? m_Services.World() : nullptr;
    if (!world || !volume.IsValid())
        return "no volume in the scene";
    const auto* name = world->GetComponent<Components::Name>(volume);
    // A volume applies no profile: say so beside the volume the window works in.
    return "on " + (name && !name->View().empty() ? std::string(name->View()) : std::string("an unnamed volume")) +
           ", which applies no profile";
}

void DayCycleWindow::Refresh()
{
    if (!m_Profile || DayCycleWorkingCopy::MostRecentGeneration() != m_ShownGeneration)
    {
        m_ShownGeneration = DayCycleWorkingCopy::MostRecentGeneration();
        std::shared_ptr<DayCycleWorkingCopy> profile = DayCycleWorkingCopy::MostRecent();
        if (!profile && m_Services.Assets)
        {
            std::string error;
            profile = DayCycleWorkingCopy::Open(*m_Services.Assets, DefaultDayCycleProfilePath(*m_Services.Assets), error);
            m_ShownGeneration = DayCycleWorkingCopy::MostRecentGeneration();
        }
        if (profile != m_Profile)
        {
            m_Profile = std::move(profile);
            m_NeedsBuild = true;
        }
    }
    if (m_Scrub)
        m_Scrub->CheckScene();
    if (m_NeedsBuild)
        Build();

    const ECS::EntityHandle volume = ContextVolume();
    if (Label* label = m_VolumeLabel.Get(); label && volume != m_ShownVolume)
        label->SetText(VolumeName(volume));
    m_ShownVolume = volume;

    ECS::World* world = m_Services.World ? m_Services.World() : nullptr;
    if (!world)
        return;
    const DayCycleSunNow now = ReadDayCycleSunNow(*world, m_Services.Render ? m_Services.Render() : nullptr,
                                                  m_Services.Sky ? m_Services.Sky() : nullptr);
    const std::string notice = m_Scrub ? m_Scrub->BlockedNotice() : std::string("No scene is open to scrub.");
    if (m_Rows)
    {
        m_Rows->SetPlayhead(now.Hours, now.ElevationDegrees, now.DuskHalf, now.HourPlacesSun);
        m_Rows->SetScrubOff(!notice.empty());
    }
    if (Label* readout = m_ScrubReadout.Get())
        readout->SetText((now.HourPlacesSun ? SolarClockText(now.Hours) + "   " : std::string("no hour: ")) + "sun " +
                         DayCycleDegreesText(now.ElevationDegrees) +
                         (notice.empty() ? "   drag the playhead to preview the hour" : ""));
    if (Label* label = m_Notice.Get())
        label->SetText(notice);
}

void DayCycleWindow::RequestBuild()
{
    // The click that asks for it comes from a control the build replaces, so the build runs after
    // the click returns.
    m_NeedsBuild = true;
    PostAction([ref = UIElement::MakeWeakRef(this)]() {
        if (DayCycleWindow* window = ref.Get())
            window->Refresh();
    });
}

void DayCycleWindow::SetScrubHeld(bool held)
{
    if (m_Scrub)
        m_Scrub->SetHeld(held);
    Refresh();
}

void DayCycleWindow::ScrubTo(float hours)
{
    if (m_Scrub)
        m_Scrub->ScrubTo(hours);
    Refresh();
}

void DayCycleWindow::Build()
{
    // The layout binds once a UI manager owns the window; until then there is nothing to fill.
    if (!IsLayoutBound())
        return;
    m_NeedsBuild = false;
    m_Rows.reset();
    m_ShownVolume = ECS::EntityHandle{};
    const bool elevationView = DayCycleSessionAxisView() == DayCycleAxisView::Elevation;

    if (Label* name = m_ProfileName.Get())
    {
        // The profile's own name: the asset path comes back case-folded from the registry.
        const std::string profileName = m_Profile ? m_Profile->GetData().Name : std::string();
        name->SetText(!m_Profile ? std::string("none: select one in the asset browser")
                                 : profileName.empty() ? m_Profile->GetPath().stem().string() : profileName);
    }
    if (UIElement* host = m_AxisHost.Get())
    {
        host->RemoveAllChildren();
        AddDayCycleAxisToggle(*host, DayCycleSessionAxisView(), [this](DayCycleAxisView view) {
            if (view == DayCycleSessionAxisView())
                return;
            DayCycleSessionAxisView() = view;
            RequestBuild();
        });
    }
    if (UIElement* host = m_SaveHost.Get())
    {
        host->RemoveAllChildren();
        if (m_Profile)
            AddDayCycleSaveButton(*host, m_Profile, {});
    }
    if (Button* presets = m_PresetsButton.Get())
        presets->SetText(m_ShowPresets ? "Presets \xE2\x96\xB4" : "Presets \xE2\x96\xBE");
    if (UIElement* host = m_PresetsHost.Get())
    {
        host->RemoveAllChildren();
        ShowWhen(host, m_ShowPresets && m_Profile);
        if (m_ShowPresets && m_Profile && m_Services.Assets)
            AddDayCyclePresets(*host, *m_Services.Assets, m_Profile, m_Services.Undo);
    }
    ShowWhen(m_DuskCaption.Get(), elevationView);

    ScrollView* scroll = m_TracksScroll.Get();
    if (!scroll || !m_Profile)
        return;
    if (!m_TracksContent.Get())
    {
        auto content = std::make_unique<UIElement>();
        content->AddClass("day-cycle-content");
        m_TracksContent = UIElement::MakeWeakRef(content.get());
        scroll->AddContent(std::move(content));
    }
    UIElement& content = *m_TracksContent.Get();
    content.RemoveAllChildren();
    DayCycleTrackRowsContext rows;
    rows.Profile = m_Profile;
    rows.Undo = m_Services.Undo;
    rows.View = DayCycleSessionAxisView();
    rows.Below = m_Below;
    if (ECS::World* world = m_Services.World ? m_Services.World() : nullptr)
        rows.Path = ReadDayCycleSunNow(*world, nullptr, nullptr).Path;
    rows.OnScrub = [this](float hours) { ScrubTo(hours); };
    rows.OnScrubHeld = [this](bool held) { SetScrubHeld(held); };
    rows.ScrubRow = true;
    m_Rows = std::make_unique<DayCycleTrackRows>(content, std::move(rows));
}

} // namespace GameEngine::Editor
