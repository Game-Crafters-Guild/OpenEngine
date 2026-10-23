#pragma once

#include "Assets/DayCycleProfile.h"
#include "Assets/DayCycleTracks.h"
#include "DayCycle/DayCycleAxis.h"
#include "Rendering/Sky/SolarPath.h"
#include "UI/Controls/CurveField.h"
#include "UI/UIElement.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine
{
class Button;
class Label;
class Toggle;
} // namespace GameEngine

namespace GameEngine::Editor
{
class DayCycleWorkingCopy;
class UndoRedoService;

/// What the track rows show and where their edits and scrubs go.
struct DayCycleTrackRowsContext
{
    std::shared_ptr<DayCycleWorkingCopy> Profile;
    UndoRedoService* Undo = nullptr;
    DayCycleAxisView View = DayCycleAxisView::Elevation;
    /// The open scene's sun path: the hours printed beside the degrees.
    Rendering::SolarFrame Path{};
    /// The profile whose values a left-out track shows, drawn gray: in the Day Cycle window the shipped
    /// Default, the day every volume starts from. Null for the profile's inspector, where an asset has no
    /// volume around it: a left-out track there reads "not keyed by this profile" and shows no value.
    std::shared_ptr<const DayCycleProfileData> Below;
    /// Called with the hour a playhead drag reaches, and when the drag starts and ends. Null: the
    /// graphs do not scrub.
    std::function<void(float hours)> OnScrub;
    std::function<void(bool held)> OnScrubHeld;
    /// Builds the scrub strip as the first row, on the tracks' own axis and plot width, so one
    /// playhead runs through the strip and every track.
    bool ScrubRow = false;
};

/// The rows of a Day Cycle profile's tracks, one per track the table declares, on one shared axis:
/// a header with the playhead's hour, elevation and value, the graph, and a column with the value
/// now, where it comes from, Separate dusk and Add or Remove track. The Day Cycle window and the
/// profile's inspector both build their rows here. The rows follow the working copy: an edit from
/// either surface reaches both.
class DayCycleTrackRows
{
  public:
    /// Builds the rows into a container added under `parent`, which owns it. Once the container is
    /// gone this object does nothing, so it may outlive the surface that built it.
    DayCycleTrackRows(UIElement& parent, DayCycleTrackRowsContext context);
    ~DayCycleTrackRows();
    DayCycleTrackRows(const DayCycleTrackRows&) = delete;
    DayCycleTrackRows& operator=(const DayCycleTrackRows&) = delete;

    /// Moves every row's playhead to the sun now: `hours` of local solar time (printed only when
    /// `hourPlacesSun`: a sun set by hand or following a light has no hour), `elevationDegrees` high,
    /// on the setting half when `duskHalf`. A playhead beyond the axis is not drawn. A row whose
    /// track was added, removed, split or joined since the last call is rebuilt here.
    void SetPlayhead(float hours, float elevationDegrees, bool duskHalf, bool hourPlacesSun);
    /// Turns the scrub strip off (dimmed, not draggable) or on.
    void SetScrubOff(bool off);

  private:
    struct Row
    {
        UIElement* Root = nullptr;
        Label* Readout = nullptr;
        CurveField* Graph = nullptr;
        Label* NowValue = nullptr;
        Label* Source = nullptr;
        Toggle* SeparateDusk = nullptr;
        // The shape the row was built for; a change of it rebuilds the row.
        bool Keyed = false;
        bool Split = false;
        bool EditsDusk = false;
    };

    void Build();
    void BuildScrubRow();
    CurveField::Config AxisConfig() const;
    void BuildRow(const DayCycleTrackDesc& desc, Row& row);
    void BuildGraph(const DayCycleTrackDesc& desc, UIElement& parent, Row& row);
    void BuildSideColumn(const DayCycleTrackDesc& desc, UIElement& parent, Row& row);
    void OnProfileChanged();
    // Rebuilds the rows once the current UI event returns.
    void RequestBuild();
    void RefreshRow(const DayCycleTrackDesc& desc, Row& row);
    // The keys a row edits, and the profile with them replaced.
    DayCycleProfileData WithKeys(DayCycleTrack track, bool dusk, const std::vector<Math::CurveKey>& keys) const;
    void SetTrackKeyed(const DayCycleTrackDesc& desc, bool keyed);
    void SetSeparateDusk(const DayCycleTrackDesc& desc, bool separate);
    float ValueAt(DayCycleTrack track, float elevationDegrees, bool duskHalf, bool& outFromBelow) const;
    float HourForGraphTime(float time) const;
    std::string ValueText(const DayCycleTrackDesc& desc, float value) const;

    UIElement* Container() const;

    UIElement::WeakRef<UIElement> m_Container;
    DayCycleTrackRowsContext m_Context;
    std::array<Row, kDayCycleTracks.size()> m_Rows{};
    std::uint64_t m_ProfileToken = 0;
    float m_Hours = 12.0f;
    float m_ElevationDegrees = 0.0f;
    bool m_DuskHalf = false;
    bool m_NeedsBuild = false;
    bool m_HourPlacesSun = true;
    bool m_ScrubOff = false;
    UIElement* m_ScrubRoot = nullptr;
    CurveField* m_ScrubGraph = nullptr;
    // Held for this object's life: a posted rebuild that finds it gone does nothing.
    std::shared_ptr<int> m_Alive = std::make_shared<int>(0);
};

} // namespace GameEngine::Editor
