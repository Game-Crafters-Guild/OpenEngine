#include "DayCycle/DayCycleTrackRows.h"

#include "DayCycle/DayCycleUi.h"
#include "DayCycle/DayCycleWorkingCopy.h"
#include "Inspectors/SkySunReadoutText.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/CurveField.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Toggle.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>

namespace GameEngine::Editor
{
namespace
{
constexpr float kRadiansPerDegree = std::numbers::pi_v<float> / 180.0f;
// Samples of the track drawn through the day in the time-of-day view, and of the twilight band.
constexpr int kDaySamples = 193;
// A value mark's label sits on its line's upper side. The zero, the value that changes nothing, is
// always labelled; an end's label is drawn only when the zero lies at least this fraction of the range
// away from that end, so the two never overlap.
constexpr float kZeroLabelMinimumEndFraction = 1.0f / 3.0f;

std::vector<Math::CurveKey> KeysOf(const Math::Curve& curve)
{
    return std::vector<Math::CurveKey>(curve.Keys, curve.Keys + curve.KeyCount);
}

Math::Curve CurveOf(const std::vector<Math::CurveKey>& keys)
{
    Math::Curve curve{};
    for (const Math::CurveKey& key : keys)
        curve.TryInsert(key);
    return curve;
}

bool SameKeys(const std::vector<Math::CurveKey>& a, const std::vector<Math::CurveKey>& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
    {
        if (a[i].Time != b[i].Time || a[i].Value != b[i].Value || a[i].InTangent != b[i].InTangent ||
            a[i].OutTangent != b[i].OutTangent || a[i].Interp != b[i].Interp || a[i].TangentMode != b[i].TangentMode)
            return false;
    }
    return true;
}

// A track's samples along the elevation axis, as a curve field's reference line.
std::vector<Math::CurveKey> ElevationSamples(const DayCycleTrackKeys& keys, bool duskHalf)
{
    std::vector<Math::CurveKey> samples;
    constexpr int kSamples = 97;
    for (int i = 0; i < kSamples; ++i)
    {
        const float degrees = kDayCycleAxisLowestDegrees + (kDayCycleAxisZenithDegrees - kDayCycleAxisLowestDegrees) *
                                                              static_cast<float>(i) / static_cast<float>(kSamples - 1);
        Math::CurveKey sample{};
        sample.Time = degrees;
        sample.Value = EvaluateDayCycleTrack(keys, degrees * kRadiansPerDegree, duskHalf);
        samples.push_back(sample);
    }
    return samples;
}

// A track drawn through the day on `path`: its value at the sun's elevation each hour, the setting
// half's keys after the sun crosses the meridian.
std::vector<Math::CurveKey> DaySamples(const DayCycleTrackKeys& keys, const Rendering::SolarFrame& path)
{
    std::vector<Math::CurveKey> samples;
    for (int i = 0; i < kDaySamples; ++i)
    {
        const float hours = kHoursPerDay * static_cast<float>(i) / static_cast<float>(kDaySamples - 1);
        Math::CurveKey sample{};
        sample.Time = hours;
        sample.Value = EvaluateDayCycleTrack(keys, ElevationDegreesAtHour(path, hours) * kRadiansPerDegree,
                                             IsDuskHour(path, hours));
        samples.push_back(sample);
    }
    return samples;
}

// The hours the sun spends in twilight on `path`, as shaded spans of the time-of-day view.
std::vector<CurveField::ShadedSpan> TwilightHours(const Rendering::SolarFrame& path)
{
    std::vector<CurveField::ShadedSpan> spans;
    bool inside = false;
    float start = 0.0f;
    for (int i = 0; i < kDaySamples; ++i)
    {
        const float hours = kHoursPerDay * static_cast<float>(i) / static_cast<float>(kDaySamples - 1);
        const float elevation = ElevationDegreesAtHour(path, hours);
        const bool twilight = elevation >= kDayCycleTwilightLowestDegrees && elevation <= 0.0f;
        if (twilight && !inside)
            start = hours;
        if (!twilight && inside)
            spans.push_back({start, hours, ""});
        inside = twilight;
    }
    if (inside)
        spans.push_back({start, kHoursPerDay, ""});
    return spans;
}

std::string TrackTitle(const DayCycleTrackDesc& desc)
{
    std::string title(desc.Label);
    if (!desc.Unit.empty())
        title += " (" + std::string(desc.Unit) + ")";
    return title;
}
} // namespace

DayCycleTrackRows::DayCycleTrackRows(UIElement& parent, DayCycleTrackRowsContext context)
    : m_Context(std::move(context))
{
    m_Container = UIElement::MakeWeakRef(AddDayCycleChild<UIElement>(parent, "day-cycle-tracks"));
    if (m_Context.Profile)
        m_ProfileToken = m_Context.Profile->Subscribe([this]() { OnProfileChanged(); });
    Build();
}

DayCycleTrackRows::~DayCycleTrackRows()
{
    if (m_Context.Profile)
        m_Context.Profile->Unsubscribe(m_ProfileToken);
}

void DayCycleTrackRows::RequestBuild()
{
    m_NeedsBuild = true;
    if (UIElement* container = Container())
        container->PostAction([alive = std::weak_ptr<int>(m_Alive), this]() {
            if (alive.lock() && m_NeedsBuild)
                Build();
        });
}

UIElement* DayCycleTrackRows::Container() const
{
    return m_Container.Get();
}

void DayCycleTrackRows::SetPlayhead(float hours, float elevationDegrees, bool duskHalf, bool hourPlacesSun)
{
    if (!Container())
        return;
    m_HourPlacesSun = hourPlacesSun;
    // A split track edits the half the playhead stands on.
    if (duskHalf != m_DuskHalf && std::any_of(m_Rows.begin(), m_Rows.end(), [](const Row& row) { return row.Split; }))
        m_NeedsBuild = true;
    m_Hours = hours;
    m_ElevationDegrees = elevationDegrees;
    m_DuskHalf = duskHalf;
    if (m_NeedsBuild)
    {
        Build();
        return;
    }
    for (const DayCycleTrackDesc& desc : kDayCycleTracks)
        RefreshRow(desc, m_Rows[static_cast<size_t>(desc.Track)]);
    if (m_ScrubGraph)
    {
        const CurveField::Config& axis = m_ScrubGraph->GetConfig();
        const float time = m_Context.View == DayCycleAxisView::Elevation ? m_ElevationDegrees : m_Hours;
        m_ScrubGraph->SetPlaybackIndicator(time, 0.0f, time >= axis.TimeMin && time <= axis.TimeMax);
    }
}

void DayCycleTrackRows::SetScrubOff(bool off)
{
    if (off == m_ScrubOff)
        return;
    m_ScrubOff = off;
    if (m_ScrubRoot)
        off ? m_ScrubRoot->AddClass("day-cycle-scrub-off") : m_ScrubRoot->RemoveClass("day-cycle-scrub-off");
    if (m_ScrubGraph)
    {
        CurveField::Config config = AxisConfig();
        config.AllowPlaybackScrub = !off;
        config.AllowTimeDrag = false;
        config.AllowAddRemove = false;
        m_ScrubGraph->SetConfig(config);
    }
}

CurveField::Config DayCycleTrackRows::AxisConfig() const
{
    const bool elevationView = m_Context.View == DayCycleAxisView::Elevation;
    const Rendering::SolarFrame path = m_Context.Path;
    CurveField::Config config;
    config.ShowPlaybackIndicator = true;
    config.ShowPlaybackValueMarker = false;
    config.ShowPlaybackTimeLabel = false;
    config.TimeAxisLabels = CurveField::TimeLabels::Formatted;
    if (elevationView)
    {
        // Through the twilight every 6 degrees, above the horizon every 10, up to the scene's own noon.
        const float noon = ElevationDegreesAtHour(path, 12.0f);
        const float highest = std::clamp(std::ceil(noon / kDayAxisStepDegrees) * kDayAxisStepDegrees,
                                         kDayAxisStepDegrees, kDayCycleAxisZenithDegrees);
        config.TimeMin = kDayCycleAxisLowestDegrees;
        config.TimeMax = highest;
        config.TimeTicks = {-18.0f, -12.0f, -6.0f, 0.0f};
        for (float tick = kDayAxisStepDegrees; tick <= highest + 1e-3f; tick += kDayAxisStepDegrees)
            config.TimeTicks.push_back(tick);
        config.TimeLabel = &DayCycleDegreesTickText;
        config.SecondaryTimeLabel = [path](float degrees) { return SolarClockText(HourAtElevation(path, degrees, true)); };
    }
    else
    {
        config.TimeMin = 0.0f;
        config.TimeMax = kHoursPerDay;
        config.TimeTicks = {0.0f, 3.0f, 6.0f, 9.0f, 12.0f, 15.0f, 18.0f, 21.0f, 24.0f};
        config.TimeLabel = &SolarClockText;
        config.SecondaryTimeLabel = [path](float hours) {
            return DayCycleDegreesTickText(ElevationDegreesAtHour(path, hours));
        };
    }
    return config;
}

void DayCycleTrackRows::BuildScrubRow()
{
    // The strip has the rows' shape (a body holding the graph and a side column), so its plot
    // starts and ends where every track's does.
    m_ScrubRoot = AddDayCycleChild<UIElement>(*Container(), "day-cycle-track");
    m_ScrubRoot->AddClass("day-cycle-scrub-row");
    if (m_ScrubOff)
        m_ScrubRoot->AddClass("day-cycle-scrub-off");
    UIElement* body = AddDayCycleChild<UIElement>(*m_ScrubRoot, "day-cycle-track-body");
    CurveField::Config config = AxisConfig();
    config.AllowTimeDrag = false;
    config.AllowAddRemove = false;
    config.AllowPlaybackScrub = !m_ScrubOff;
    m_ScrubGraph = AddDayCycleChild<CurveField>(*body, "day-cycle-track-graph");
    m_ScrubGraph->AddClass("day-cycle-scrub-graph");
    m_ScrubGraph->SetConfig(config);
    m_ScrubGraph->SetOnPlaybackScrub([this](float time) { m_Context.OnScrub(HourForGraphTime(time)); });
    m_ScrubGraph->SetOnPlaybackScrubActiveChanged([this](bool held) {
        if (m_Context.OnScrubHeld)
            m_Context.OnScrubHeld(held);
    });
    AddDayCycleChild<UIElement>(*body, "day-cycle-track-side");
}

void DayCycleTrackRows::Build()
{
    m_NeedsBuild = false;
    UIElement* container = Container();
    if (!container)
        return;
    container->RemoveAllChildren();
    m_ScrubRoot = nullptr;
    m_ScrubGraph = nullptr;
    for (Row& row : m_Rows)
        row = Row{};
    if (m_Context.ScrubRow && m_Context.OnScrub)
        BuildScrubRow();
    for (const DayCycleTrackDesc& desc : kDayCycleTracks)
        BuildRow(desc, m_Rows[static_cast<size_t>(desc.Track)]);
}

void DayCycleTrackRows::BuildRow(const DayCycleTrackDesc& desc, Row& row)
{
    const DayCycleTrackKeys* keys = m_Context.Profile ? m_Context.Profile->GetData().Find(desc.Track) : nullptr;
    row.Keyed = keys != nullptr;
    row.Split = keys && keys->SeparateDusk;
    row.EditsDusk = row.Split && m_DuskHalf;

    row.Root = AddDayCycleChild<UIElement>(*Container(), "day-cycle-track");
    UIElement* header = AddDayCycleChild<UIElement>(*row.Root, "day-cycle-track-header");
    AddDayCycleText(*header, TrackTitle(desc), "day-cycle-track-title");
    if (row.Split && m_Context.View == DayCycleAxisView::Elevation)
        AddDayCycleText(*header,
                        std::string("Blue: the ") + (row.EditsDusk ? "dusk" : "dawn") +
                            " half, where the playhead is and where you edit. Yellow: the " +
                            (row.EditsDusk ? "dawn" : "dusk") + " half.",
                        "day-cycle-track-legend");
    row.Readout = AddDayCycleText(*header, "", "day-cycle-track-readout");

    UIElement* body = AddDayCycleChild<UIElement>(*row.Root, "day-cycle-track-body");
    if (!row.Keyed && !m_Context.Below)
    {
        // With nothing below the profile, a left-out track has no value to show: it is not part of this profile.
        UIElement* empty = AddDayCycleChild<UIElement>(*body, "day-cycle-track-empty");
        AddDayCycleText(*empty, "not keyed by this profile", "day-cycle-track-empty-text");
        Button* add = AddDayCycleChild<Button>(*empty, "day-cycle-track-action");
        add->SetText("Add track");
        add->SetOnClick([this, &desc](UIEvent&) { SetTrackKeyed(desc, true); });
        RefreshRow(desc, row);
        return;
    }
    BuildGraph(desc, *body, row);
    BuildSideColumn(desc, *body, row);
    RefreshRow(desc, row);
}

void DayCycleTrackRows::BuildGraph(const DayCycleTrackDesc& desc, UIElement& parent, Row& row)
{
    const bool elevationView = m_Context.View == DayCycleAxisView::Elevation;
    const bool editable = elevationView && row.Keyed;
    const Rendering::SolarFrame path = m_Context.Path;

    CurveField::Config config = AxisConfig();
    config.ValueMin = desc.DisplayMin;
    config.ValueMax = desc.DisplayMax;
    config.ValueAxisMarks = {desc.DisplayMin, 0.0f, desc.DisplayMax};
    const float zeroFraction = -desc.DisplayMin / (desc.DisplayMax - desc.DisplayMin);
    const bool labelMin = zeroFraction >= kZeroLabelMinimumEndFraction;
    const bool labelMax = zeroFraction <= 1.0f - kZeroLabelMinimumEndFraction;
    config.ValueLabel = [labelMin, labelMax, min = desc.DisplayMin, max = desc.DisplayMax](float value) {
        if (value == 0.0f)
            return std::string("0");
        if ((value == min && !labelMin) || (value == max && !labelMax))
            return std::string();
        char text[16];
        std::snprintf(text, sizeof(text), "%+.1f", static_cast<double>(value));
        return std::string(text);
    };
    config.AllowTimeDrag = editable;
    config.AllowAddRemove = editable;
    config.AllowTangentEditing = editable;
    config.MinKeys = 1;
    config.AllowPlaybackScrub = static_cast<bool>(m_Context.OnScrub);

    row.Graph = AddDayCycleChild<CurveField>(parent, "day-cycle-track-graph");
    // Gray is the value from below; the other half of a split track and the curve drawn through the
    // day take the profile's own colors.
    if (row.Keyed && row.Split && elevationView)
        row.Graph->AddClass("day-cycle-track-graph-split");
    if (row.Keyed && !elevationView)
        row.Graph->AddClass("day-cycle-track-graph-day");
    row.Graph->SetConfig(config);
    row.Graph->SetShadedSpans(elevationView
                                  ? std::vector<CurveField::ShadedSpan>{{kDayCycleTwilightLowestDegrees, 0.0f, ""}}
                                  : TwilightHours(path));

    if (m_Context.OnScrub)
    {
        row.Graph->SetOnPlaybackScrub([this](float time) { m_Context.OnScrub(HourForGraphTime(time)); });
        row.Graph->SetOnPlaybackScrubActiveChanged([this](bool held) {
            if (m_Context.OnScrubHeld)
                m_Context.OnScrubHeld(held);
        });
    }
    if (!editable)
        return;
    const DayCycleTrack track = desc.Track;
    const bool dusk = row.EditsDusk;
    row.Graph->SetOnChanging([this, track, dusk](const std::vector<Math::CurveKey>& keys) {
        m_Context.Profile->Preview(WithKeys(track, dusk, keys));
    });
    const std::string label = "Edit " + std::string(desc.Label) + " keys";
    row.Graph->SetOnChanged([this, track, dusk, label](const std::vector<Math::CurveKey>& keys) {
        m_Context.Profile->Commit(WithKeys(track, dusk, keys), label, m_Context.Undo);
    });
}

void DayCycleTrackRows::BuildSideColumn(const DayCycleTrackDesc& desc, UIElement& parent, Row& row)
{
    UIElement* side = AddDayCycleChild<UIElement>(parent, "day-cycle-track-side");
    AddDayCycleText(*side, "now", "day-cycle-track-side-caption");
    row.NowValue = AddDayCycleText(*side, "", "day-cycle-track-now");
    row.Source = AddDayCycleText(*side, "", "day-cycle-track-source");
    if (row.Keyed)
    {
        UIElement* splitRow = AddDayCycleChild<UIElement>(*side, "day-cycle-track-split");
        row.SeparateDusk = AddDayCycleChild<Toggle>(*splitRow, "day-cycle-track-split-toggle");
        row.SeparateDusk->SetValueWithoutNotify(row.Split);
        row.SeparateDusk->SetOnValueChanged([this, &desc](const bool& on) { SetSeparateDusk(desc, on); });
        AddDayCycleText(*splitRow, "Separate dusk", "day-cycle-track-split-label");
    }
    Button* action = AddDayCycleChild<Button>(*side, "day-cycle-track-action");
    action->SetText(row.Keyed ? "Remove track" : "Add track");
    const bool keyed = row.Keyed;
    action->SetOnClick([this, &desc, keyed](UIEvent&) { SetTrackKeyed(desc, !keyed); });
    if (m_Context.View == DayCycleAxisView::TimeOfDay && row.Keyed)
        AddDayCycleText(*side, "Edit the keys in the Sun's elevation view.", "day-cycle-track-side-caption");
}

void DayCycleTrackRows::OnProfileChanged()
{
    if (!Container())
        return;
    const DayCycleProfileData& data = m_Context.Profile->GetData();
    for (const DayCycleTrackDesc& desc : kDayCycleTracks)
    {
        const Row& row = m_Rows[static_cast<size_t>(desc.Track)];
        const DayCycleTrackKeys* keys = data.Find(desc.Track);
        // A track added, removed, split or joined changes the row's controls. The change may come
        // from one of those controls, which must outlive its own click, so the rows are rebuilt
        // right after the click returns (or, with no UI to post to, at the next playhead update).
        if ((keys != nullptr) != row.Keyed || (keys && keys->SeparateDusk != row.Split))
        {
            RequestBuild();
            return;
        }
    }
    for (const DayCycleTrackDesc& desc : kDayCycleTracks)
        RefreshRow(desc, m_Rows[static_cast<size_t>(desc.Track)]);
}

void DayCycleTrackRows::RefreshRow(const DayCycleTrackDesc& desc, Row& row)
{
    bool fromBelow = false;
    const float value = ValueAt(desc.Track, m_ElevationDegrees, m_DuskHalf, fromBelow);
    // A left-out track with nothing below the profile has no value: the readout gives the sun only.
    const bool valueKnown = !fromBelow || m_Context.Below;
    if (row.Readout)
        row.Readout->SetText((m_HourPlacesSun ? SolarClockText(m_Hours) + "   " : std::string()) + "sun " +
                             DayCycleDegreesText(m_ElevationDegrees) +
                             (valueKnown ? "   " + ValueText(desc, value) : std::string()));
    if (row.NowValue)
        row.NowValue->SetText(ValueText(desc, value));
    if (row.Source)
        row.Source->SetText(fromBelow ? "not keyed: the Default profile's value" : "keyed by this profile");
    if (!row.Graph)
        return;

    const DayCycleTrackKeys* keys = m_Context.Profile ? m_Context.Profile->GetData().Find(desc.Track) : nullptr;
    const DayCycleTrackKeys* below = m_Context.Below ? m_Context.Below->Find(desc.Track) : nullptr;
    const bool elevationView = m_Context.View == DayCycleAxisView::Elevation;
    if (elevationView && keys)
    {
        const std::vector<Math::CurveKey> edited = KeysOf(row.EditsDusk ? keys->DuskKeys : keys->Keys);
        if (!SameKeys(edited, row.Graph->GetKeys()))
            row.Graph->SetKeys(edited);
        // A split track draws the half it does not edit behind the one it edits.
        row.Graph->SetReferenceLine(row.Split ? ElevationSamples(*keys, !row.EditsDusk) : std::vector<Math::CurveKey>{});
    }
    else if (elevationView)
    {
        row.Graph->SetReferenceLine(below ? ElevationSamples(*below, m_DuskHalf) : std::vector<Math::CurveKey>{});
    }
    else
    {
        const DayCycleTrackKeys* drawn = keys ? keys : below;
        row.Graph->SetReferenceLine(drawn ? DaySamples(*drawn, m_Context.Path) : std::vector<Math::CurveKey>{});
    }
    const float time = elevationView ? m_ElevationDegrees : m_Hours;
    const CurveField::Config& axis = row.Graph->GetConfig();
    row.Graph->SetPlaybackIndicator(time, value, time >= axis.TimeMin && time <= axis.TimeMax);
}

DayCycleProfileData DayCycleTrackRows::WithKeys(DayCycleTrack track, bool dusk,
                                                const std::vector<Math::CurveKey>& keys) const
{
    DayCycleProfileData data = m_Context.Profile->GetData();
    if (DayCycleTrackKeys* edited = data.Find(track))
        (dusk ? edited->DuskKeys : edited->Keys) = CurveOf(keys);
    return data;
}

void DayCycleTrackRows::SetTrackKeyed(const DayCycleTrackDesc& desc, bool keyed)
{
    DayCycleProfileData data = m_Context.Profile->GetData();
    if (!keyed)
    {
        std::erase_if(data.Tracks, [&desc](const DayCycleTrackKeys& keys) { return keys.Track == desc.Track; });
        m_Context.Profile->Commit(data, "Remove " + std::string(desc.Label) + " track", m_Context.Undo);
        return;
    }
    DayCycleTrackKeys added;
    added.Track = desc.Track;
    // A new track starts from the value below it, so adding it changes nothing until it is edited.
    if (const DayCycleTrackKeys* below = m_Context.Below ? m_Context.Below->Find(desc.Track) : nullptr)
        added = *below;
    else
        added.Keys.TryInsert({0.0f, desc.Identity});
    auto at = data.Tracks.begin();
    while (at != data.Tracks.end() && at->Track < desc.Track)
        ++at;
    data.Tracks.insert(at, added);
    m_Context.Profile->Commit(data, "Add " + std::string(desc.Label) + " track", m_Context.Undo);
}

void DayCycleTrackRows::SetSeparateDusk(const DayCycleTrackDesc& desc, bool separate)
{
    DayCycleProfileData data = m_Context.Profile->GetData();
    DayCycleTrackKeys* keys = data.Find(desc.Track);
    if (!keys || keys->SeparateDusk == separate)
        return;
    keys->SeparateDusk = separate;
    // The dusk starts as a copy of the day, so splitting it changes nothing until one half is edited.
    keys->DuskKeys = separate ? keys->Keys : Math::Curve{};
    m_Context.Profile->Commit(data, std::string(separate ? "Separate " : "Join ") + "the dusk of " +
                                        std::string(desc.Label),
                              m_Context.Undo);
}

float DayCycleTrackRows::ValueAt(DayCycleTrack track, float elevationDegrees, bool duskHalf, bool& outFromBelow) const
{
    const float elevation = elevationDegrees * kRadiansPerDegree;
    outFromBelow = false;
    if (m_Context.Profile)
    {
        if (const DayCycleTrackKeys* keys = m_Context.Profile->GetData().Find(track))
            return EvaluateDayCycleTrack(*keys, elevation, duskHalf);
    }
    outFromBelow = true;
    if (const DayCycleTrackKeys* below = m_Context.Below ? m_Context.Below->Find(track) : nullptr)
        return EvaluateDayCycleTrack(*below, elevation, duskHalf);
    return GetDayCycleTrackDesc(track).Identity;
}

float DayCycleTrackRows::HourForGraphTime(float time) const
{
    if (m_Context.View == DayCycleAxisView::TimeOfDay)
        return time;
    // The elevation view scrubs along the half a split track edits; an unsplit day along the dusk.
    const bool anySplitEditingDawn = std::any_of(m_Rows.begin(), m_Rows.end(),
                                                 [](const Row& row) { return row.Split && !row.EditsDusk; });
    return HourAtElevation(m_Context.Path, time, !anySplitEditingDawn);
}

std::string DayCycleTrackRows::ValueText(const DayCycleTrackDesc& desc, float value) const
{
    char text[32];
    std::snprintf(text, sizeof(text), "%+.2f", static_cast<double>(value));
    return desc.Unit.empty() ? std::string(text) : std::string(text) + " " + std::string(desc.Unit);
}

} // namespace GameEngine::Editor
