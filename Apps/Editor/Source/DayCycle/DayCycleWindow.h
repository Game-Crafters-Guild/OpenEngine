#pragma once

#include "DayCycle/DayCycleEditorServices.h"
#include "ECS/Entity.h"
#include "UI/AssetBoundDockPanel.h"
#include "UI/UIManagerRef.h"

#include <cstdint>
#include <memory>
#include <string>

namespace GameEngine
{
class Button;
class CurveField;
class Label;
class ScrollView;
struct DayCycleProfileData;
} // namespace GameEngine

namespace GameEngine::Editor
{
class DayCycleScrub;
class DayCycleTrackRows;
class DayCycleWorkingCopy;

/// The Day Cycle window: the profile the editor last opened (the asset selected in the asset browser,
/// or Open in the Day Cycle window on its inspector), else the shipped Default, edited against the
/// shipped Default where it leaves a track out. Its static layout is UI/panels/DayCycleWindow.uxml: a
/// toolbar with the profile and the volume it is shown on, the axis toggle, Save and the presets, and
/// the scrub's readout; the scrub strip and the tracks are built here from the track table, on one
/// shared axis with one playhead through all of them. The
/// window and the profile's inspector edit one working copy.
class DayCycleWindow final : public AssetBoundDockPanel
{
  public:
    explicit DayCycleWindow(DayCycleEditorServices services);
    ~DayCycleWindow() override;

    /// The volume the window works in: the selected Post Process Volume, else the world's first
    /// global one. Invalid when there is neither.
    ECS::EntityHandle ContextVolume() const;
    /// The profile shown.
    const std::shared_ptr<DayCycleWorkingCopy>& GetProfile() const { return m_Profile; }
    /// Follows what changed (the profile, the view, the volume, the scene and play mode) and moves
    /// every playhead to the sun now. Runs on the UI's periodic refresh and after every scrub.
    void Refresh();

    /// The playhead was pressed or released, and dragged to `hours` of local solar time.
    void SetScrubHeld(bool held);
    void ScrubTo(float hours);

    /// Opens the scrub when the window is shown and ends it (the preview with it) when the window
    /// is closed: a closed dock tab leaves its UI manager.
    void OnOwnerManagerChanged(UIManager* owner) override;

  private:
    void OnLayoutBound() override;
    void OnLayoutReconciled() override;
    void BindElements();
    void Build();
    void RequestBuild();
    void OpenScrub();
    std::string VolumeName(ECS::EntityHandle volume) const;

    DayCycleEditorServices m_Services;
    std::shared_ptr<DayCycleWorkingCopy> m_Profile;
    std::shared_ptr<const DayCycleProfileData> m_Below;
    std::unique_ptr<DayCycleScrub> m_Scrub;
    std::unique_ptr<DayCycleTrackRows> m_Rows;
    std::uint64_t m_ShownGeneration = ~std::uint64_t{0};
    ECS::EntityHandle m_ShownVolume{};
    bool m_ShowPresets = false;
    bool m_NeedsBuild = true;

    // The layout's elements, resolved after each bind.
    WeakRef<Label> m_ProfileName;
    WeakRef<Label> m_VolumeLabel;
    WeakRef<UIElement> m_AxisHost;
    WeakRef<UIElement> m_SaveHost;
    WeakRef<Button> m_PresetsButton;
    WeakRef<UIElement> m_PresetsHost;
    WeakRef<Label> m_ScrubReadout;
    WeakRef<Label> m_Notice;
    WeakRef<Label> m_DuskCaption;
    WeakRef<ScrollView> m_TracksScroll;
    // The rows' container inside the tracks scroll, made at the first build after a bind.
    WeakRef<UIElement> m_TracksContent;

    UIManagerRef m_RefreshManager;
    std::uint64_t m_RefreshToken = 0;
};

} // namespace GameEngine::Editor
