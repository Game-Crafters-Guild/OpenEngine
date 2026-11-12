#include "Inspectors/SplineRecipe/Paving/SplinePavingInspector.h"

#include "Editor/Entities/EntityDisplayName.h"
#include "EditorContext.h"
#include "InspectorRegistry.h"
#include "Inspectors/DefaultComponentInspector.h"
#include "Inspectors/InspectorComponent/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorComponent/InspectorComponentSection.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Inspectors/Spline/SplineInspector.h"
#include "Inspectors/SplineRecipe/Paving/SplinePavingSettingRows.h"
#include "Inspectors/SplineRecipe/SplineRecipeStateNotice.h"
#include "PlayMode/PlayModeManager.h"
#include "UI/Controls/EntityMemberList/EntityMemberList.h"
#include "UI/Controls/Foldout.h"
#include "UI/Controls/SplineJunctionList/SplineJunctionList.h"
#include "UI/InspectorSection.h"
#include "UndoRedo/Spline/SplineNetworkRecordCommand.h"
#include "UndoRedo/UndoRedoService.h"

#include "Components/Spline/SplineComponent.h"
#include "Components/Spline/SplineNetworkMember.h"
#include "Components/Spline/SplinePaving.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/Vector2.h"
#include "Spline/SplineData.h"
#include "SplineECS/SplineNetwork.h"
#include "SplineECS/SplineService.h"
#include "SplineRecipes/Paving/PavingStatus.h"
#include "SplineRecipes/RecipeControllers.h"
#include "SplineRecipes/SplineRecipesScriptApi.h"
#include "Types/FormatNumber.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
namespace
{

using Components::SplinePaving;
using Components::SplinePavingMember;
using Components::SplinePavingMemberMode;
using SplineRecipes::PavingStatus;
using SplineRecipes::PavingJunctionState;
using SplineRecipes::PavingJunctionStatus;
using Mathematics::Vector2;

constexpr std::array<std::string_view, 2> kSurfaceFields = {"Material", "Seed"};
constexpr std::array<std::string_view, 1> kCurbMaterialField = {"BorderMaterial"};
constexpr const char* kCurbMaterialEmptyText = "The paving's material";
constexpr std::array<std::string_view, 2> kShadowFields = {"CastShadows", "ReceiveShadows"};
// A height difference reads to a hundredth of a meter, as the notice gives it.
constexpr int kHeightDecimals = 2;
// A junction's distance along a road reads to the meter.
constexpr int kStationDecimals = 0;

// The paving's status as the running recipe system's last rebuild left it; null in a process
// with no recipe system.
const PavingStatus* RunningPavingStatus(ECS::EntityHandle entity)
{
    const SplineRecipes::RecipeControllers* controllers = RunningRecipeControllers();
    return controllers ? controllers->PavingStatusOf(entity) : nullptr;
}

// What a spline is, as the member list reads it: closed or open, a Road of the network or not,
// and its points' bounds on the ground.
struct SplineShape
{
    bool Live = false;
    bool Closed = false;
    bool Road = false;
    Vector2 Min{std::numeric_limits<float32>::max(), std::numeric_limits<float32>::max()};
    Vector2 Max{std::numeric_limits<float32>::lowest(), std::numeric_limits<float32>::lowest()};
};

SplineShape ReadShape(const ECS::World& world, ECS::EntityHandle entity)
{
    SplineShape shape;
    const auto* spline = world.IsValid(entity) ? world.GetComponent<Components::SplineComponent>(entity) : nullptr;
    const auto* transform = spline ? world.GetComponent<Components::WorldTransform>(entity) : nullptr;
    const SplineECS::SplineService* splines = SplineECS::SplineService::TryGet();
    const Spline::SplineData* data =
        transform && splines
            ? splines->GetSplineData(SplineECS::SplineHandle(spline->SplineDataIndex, spline->SplineDataGeneration))
            : nullptr;
    if (!data || !data->IsValid())
        return shape;
    shape.Live = true;
    shape.Closed = data->IsEffectivelyClosed();
    const auto* member = world.GetComponent<Components::SplineNetworkMember>(entity);
    shape.Road = member && member->Kind == Components::SplineNetworkMemberKind::Road;
    const Mathematics::Matrix4x4 worldMatrix =
        transform->ToMatrix4x4(transform->GetPosition() - Mathematics::WorldPosition{});
    for (const Spline::SplineControlPoint& point : data->Points)
    {
        const Mathematics::Vector3 position = worldMatrix.TransformPoint(point.Position);
        shape.Min = Vector2(std::min(shape.Min.x, position.x), std::min(shape.Min.y, position.z));
        shape.Max = Vector2(std::max(shape.Max.x, position.x), std::max(shape.Max.y, position.z));
    }
    return shape;
}

bool Overlap(const SplineShape& a, const SplineShape& b)
{
    return a.Min.x <= b.Max.x && b.Min.x <= a.Max.x && a.Min.y <= b.Max.y && b.Min.y <= a.Max.y;
}

// The modes a member offers: a closed spline that is not a Road is cut out, a Road keeps its own
// surface, and only a closed Road offers both.
std::vector<EditorUI::EntityMemberList::Mode> ModesFor(const SplineShape& shape)
{
    const EditorUI::EntityMemberList::Mode exclude{static_cast<uint8>(SplinePavingMemberMode::Exclude), "Cut out",
                                                   "Cuts its area out of the paving, with the curb around it", false};
    const EditorUI::EntityMemberList::Mode ignore{static_cast<uint8>(SplinePavingMemberMode::Ignore), "Own surface",
                                                  "Keeps the road's own surface where it meets this paving", false};
    if (shape.Road && shape.Closed)
        return {exclude, ignore};
    if (shape.Road)
        return {ignore};
    return {exclude};
}

std::vector<SplinePavingMember> CurrentMembers(const SplinePaving& recipe)
{
    return std::vector<SplinePavingMember>(
        recipe.Members, recipe.Members + std::min(recipe.MemberCount, Components::kMaxPavingMembers));
}

std::vector<SplinePavingMember> CurrentMembersOf(const InspectorContext& ctx)
{
    const auto* recipe = ctx.World->GetComponent<SplinePaving>(ctx.Entity);
    return recipe ? CurrentMembers(*recipe) : std::vector<SplinePavingMember>{};
}

// Writes `members` as the paving's list, one undo step named `label`.
void CommitMembers(const InspectorContext& ctx, const std::vector<SplinePavingMember>& members, const char* label)
{
    InspectorDrag::CommitComponentWithUndo<SplinePaving>(
        ctx.World, ctx.Entity, ctx.ChangeNotifications, ctx.Undo, label, [&members](SplinePaving& edited) {
            edited.MemberCount =
                static_cast<uint32>(std::min<std::size_t>(members.size(), Components::kMaxPavingMembers));
            std::fill(std::begin(edited.Members), std::end(edited.Members), SplinePavingMember{});
            std::copy_n(members.begin(), edited.MemberCount, edited.Members);
        });
    if (ctx.RequestInspectorRefresh)
        ctx.RequestInspectorRefresh();
}

auto IsMemberEntry(ECS::EntityHandle entity)
{
    return [entity](const SplinePavingMember& member) { return member.Entity == entity; };
}

void AddMember(const InspectorContext& ctx, ECS::EntityHandle entity)
{
    std::vector<SplinePavingMember> members = CurrentMembersOf(ctx);
    if (std::any_of(members.begin(), members.end(), IsMemberEntry(entity)))
        return;
    const auto mode = static_cast<SplinePavingMemberMode>(ModesFor(ReadShape(*ctx.World, entity)).front().Value);
    members.push_back({entity, mode});
    CommitMembers(ctx, members, "Leave Out of Paving");
}

void RemoveMember(const InspectorContext& ctx, ECS::EntityHandle entity)
{
    std::vector<SplinePavingMember> members = CurrentMembersOf(ctx);
    const auto found = std::find_if(members.begin(), members.end(), IsMemberEntry(entity));
    if (found == members.end())
        return;
    members.erase(found);
    CommitMembers(ctx, members, "Pave Again");
}

void SwitchMember(const InspectorContext& ctx, ECS::EntityHandle entity, uint8 mode)
{
    std::vector<SplinePavingMember> members = CurrentMembersOf(ctx);
    const auto found = std::find_if(members.begin(), members.end(), IsMemberEntry(entity));
    if (found == members.end() || found->Mode == static_cast<SplinePavingMemberMode>(mode))
        return;
    found->Mode = static_cast<SplinePavingMemberMode>(mode);
    CommitMembers(ctx, members, "Switch Paving Mode");
}

// The words beside a row's switch: what the paving does with the spline.
std::string StateNote(const SplineShape& shape, const SplinePavingMember* member)
{
    if (!member)
        return shape.Road ? "paved where it meets" : "paved";
    return member->Mode == SplinePavingMemberMode::Ignore ? "its own surface where it meets" : "cut out";
}

// The row of `entity`: a member (switched off) when `member` is set, else a spline the paving paves.
// Only a member that offers two modes shows them.
EditorUI::EntityMemberList::Row MemberRow(const ECS::World& world, ECS::EntityHandle entity,
                                          const SplinePavingMember* member)
{
    const SplineShape shape = ReadShape(world, entity);
    EditorUI::EntityMemberList::Row row;
    row.Entity = entity;
    row.On = member == nullptr;
    row.Member = member != nullptr;
    row.Live = shape.Live;
    if (member && ModesFor(shape).size() > 1)
        row.Modes = ModesFor(shape);
    if (member)
        row.CurrentMode = static_cast<uint8>(member->Mode);
    if (row.Live)
    {
        row.Title = Editor::EntityDisplayName(world, entity);
        row.Note = StateNote(shape, member);
    }
    return row;
}

// Whether `entity` is the paving's own or a row of `content` already.
bool Listed(ECS::EntityHandle self, const EditorUI::EntityMemberList::Content& content, ECS::EntityHandle entity)
{
    const auto isEntity = [entity](const EditorUI::EntityMemberList::Row& row) { return row.Entity == entity; };
    return entity == self || std::any_of(content.Rows.begin(), content.Rows.end(), isEntity);
}

// Rows by name, then by entity, with deleted members last: a switch never moves a row.
bool RowBefore(const EditorUI::EntityMemberList::Row& a, const EditorUI::EntityMemberList::Row& b)
{
    if (a.Live != b.Live)
        return a.Live;
    if (a.Title != b.Title)
        return a.Title < b.Title;
    return a.Entity.id < b.Entity.id;
}

// A switched-on road whose every junction here has heights that differ says that it is not paved.
void NoteHeightsDiffer(EditorUI::EntityMemberList::Content& content, const PavingStatus& status)
{
    for (EditorUI::EntityMemberList::Row& row : content.Rows)
    {
        bool meets = false;
        bool allDiffer = true;
        for (const PavingJunctionStatus& junction : status.Junctions)
        {
            if (junction.Road != row.Entity)
                continue;
            meets = true;
            allDiffer &= junction.State == PavingJunctionState::HeightsDiffer;
        }
        if (row.On && row.Live && meets && allDiffer)
            row.Note = "not paved: heights differ (see Junctions)";
    }
}

// A section of the paving's inspector that holds one list; the header's tooltip says what the list does.
UIElement* AddListSection(const InspectorContext& context, std::string_view key, std::string_view title,
                          const std::string& tooltip)
{
    Foldout* section = InspectorUI::AddComponentSection(context.Parent, key, title);
    section->GetHeader()->SetTooltip(tooltip);
    return section->GetContentContainer();
}

bool IsInPlayMode(const InspectorContext& ctx)
{
    return ctx.EditorCtx && ctx.EditorCtx->PlayMode && ctx.EditorCtx->PlayMode->IsPlayingOrPaused();
}

// The members (the splines the paving leaves out), every closed spline whose bounds overlap the
// paving's and every road that meets it, by name. A row's toggle is on when it is paved.
EditorUI::EntityMemberList::Content CollectMemberRows(const InspectorContext& ctx, const SplinePaving& recipe,
                                                      const PavingStatus* status)
{
    EditorUI::EntityMemberList::Content content;
    content.Editable = !IsInPlayMode(ctx);
    content.Text.Empty = "No road meets this paving, and no closed spline overlaps it.";
    content.Text.DeletedTitle = "Deleted spline";
    content.Text.MemberTooltip = "Switch on to pave it.";
    content.Text.CandidateTooltip = "Switch off to stop paving it.";
    const std::vector<SplinePavingMember> members = CurrentMembers(recipe);
    if (members.size() >= Components::kMaxPavingMembers)
        content.AddRefusal = "A paving holds at most " + std::to_string(Components::kMaxPavingMembers) +
                             " left-out splines; pave one again first";
    for (const SplinePavingMember& member : members)
        content.Rows.push_back(MemberRow(*ctx.World, member.Entity, &member));

    if (status)
    {
        for (const PavingJunctionStatus& junction : status->Junctions)
            if (!Listed(ctx.Entity, content, junction.Road))
                content.Rows.push_back(MemberRow(*ctx.World, junction.Road, nullptr));
        NoteHeightsDiffer(content, *status);
    }
    const SplineShape self = ReadShape(*ctx.World, ctx.Entity);
    if (!self.Live)
    {
        std::sort(content.Rows.begin(), content.Rows.end(), RowBefore);
        return content;
    }
    ctx.World->Query<ECS::Read<Components::SplineComponent>>().Each(
        [&](ECS::EntityHandle entity, const Components::SplineComponent&) {
            if (Listed(ctx.Entity, content, entity))
                return;
            const SplineShape shape = ReadShape(*ctx.World, entity);
            if (shape.Live && shape.Closed && !shape.Road && Overlap(self, shape))
                content.Rows.push_back(MemberRow(*ctx.World, entity, nullptr));
        });
    std::sort(content.Rows.begin(), content.Rows.end(), RowBefore);
    return content;
}

// A switch on paves the spline again; a switch off leaves it out.
void SetPaved(const InspectorContext& ctx, ECS::EntityHandle entity, bool paved)
{
    if (paved)
        RemoveMember(ctx, entity);
    else
        AddMember(ctx, entity);
}

void AddMemberList(const InspectorContext& context, const SplinePaving& recipe, const PavingStatus* status)
{
    EditorUI::EntityMemberList::Handlers handlers;
    handlers.SetOn = [context](ECS::EntityHandle entity, bool on) { SetPaved(context, entity, on); };
    handlers.SetMode = [context](ECS::EntityHandle entity, uint8 mode) { SwitchMember(context, entity, mode); };
    handlers.Select = context.SelectEntity;
    UIElement* section = AddListSection(context, "SplinePaving/Splines", "Splines It Touches",
                                        "Switch a row off to stop paving that spline: a closed spline is cut out, "
                                        "a road keeps its own surface where it meets");
    InspectorUI::AddControl(
        section, std::make_unique<EditorUI::EntityMemberList>(CollectMemberRows(context, recipe, status), handlers));
}

// A junction whose Blend seam draws as a hard cut names the seam it gets.
constexpr const char* kHardCutSeamWords = " \xC2\xB7 Hard Cut: the road's paving ends on the seam";

std::string_view PlaceWords(SplineECS::SplineNodeKind kind)
{
    switch (kind)
    {
    case SplineECS::SplineNodeKind::Entry:
        return "entry";
    case SplineECS::SplineNodeKind::Attachment:
        return "T junction";
    case SplineECS::SplineNodeKind::Merge:
        return "end to end";
    case SplineECS::SplineNodeKind::Crossing:
        break;
    }
    return "crossing";
}

EditorUI::SplineJunctionList::Row JunctionRow(const ECS::World& world, const PavingJunctionStatus& junction)
{
    EditorUI::SplineJunctionList::Row row;
    row.NodeId = junction.NodeId;
    row.Other = junction.Road;
    row.Title = Editor::EntityDisplayName(world, junction.Road) + " \xC2\xB7 " + std::string(PlaceWords(junction.Kind));
    row.State = PavingJunctionStateWords(world, junction);
    if (junction.SeamDrawsHardCut)
        row.State += kHardCutSeamWords;
    switch (junction.State)
    {
    case PavingJunctionState::HeightsDiffer:
        row.Warning = true;
        row.Action = "Join at Grade";
        row.ActionTooltip = "Pave the road into this spline where they meet, at this spline's height";
        break;
    case PavingJunctionState::TooNarrow:
        row.Warning = true;
        break;
    case PavingJunctionState::Paved:
    case PavingJunctionState::PavedByOther:
    case PavingJunctionState::Ignored:
    case PavingJunctionState::CutOut:
        break;
    }
    return row;
}

// "Join <road> at Grade" on the junction `nodeId` of the inspected spline, its road read from
// `rows`, one undo step.
void JoinAtGrade(const InspectorContext& ctx, const std::vector<EditorUI::SplineJunctionList::Row>& rows,
                 uint32 nodeId)
{
    auto* network = SplineECS::SplineNetwork::TryGet();
    const auto row = std::find_if(rows.begin(), rows.end(),
                                  [nodeId](const EditorUI::SplineJunctionList::Row& each) { return each.NodeId == nodeId; });
    if (!network || !ctx.Undo || row == rows.end())
        return;
    const ECS::EntityHandle road = row->Other;
    for (const SplineECS::SplineNode* node : network->FindNodes(*ctx.World, ctx.Entity))
    {
        if (node->Id != nodeId)
            continue;
        if (auto command = Editor::SplineNetworkRecordCommand::JoinAtGrade(*ctx.World, *node, road))
            ctx.Undo->Execute(std::move(command));
        break;
    }
    if (ctx.RequestInspectorRefresh)
        ctx.RequestInspectorRefresh();
}

// Rows whose titles match get the junction's distance along the other spline, so each row names one place.
void NameMatchingJunctions(std::vector<EditorUI::SplineJunctionList::Row>& rows,
                           const std::vector<PavingJunctionStatus>& junctions)
{
    std::vector<std::string> titles;
    for (const EditorUI::SplineJunctionList::Row& row : rows)
        titles.push_back(row.Title);
    for (std::size_t r = 0; r < rows.size(); ++r)
    {
        if (std::count(titles.begin(), titles.end(), titles[r]) > 1)
            rows[r].Title += " at " + FormatMeters(junctions[r].RoadMeters, kStationDecimals);
    }
}

// Junction rows by title, then by junction id.
bool JunctionBefore(const EditorUI::SplineJunctionList::Row& a, const EditorUI::SplineJunctionList::Row& b)
{
    if (a.Title != b.Title)
        return a.Title < b.Title;
    return a.NodeId < b.NodeId;
}

void AddJunctionList(const InspectorContext& context, const PavingStatus* status)
{
    EditorUI::SplineJunctionList::Content content;
    content.Editable = !IsInPlayMode(context);
    content.Empty = "No road joins this paving. To join one, drag a road's end onto the paving.";
    if (status)
    {
        for (const PavingJunctionStatus& junction : status->Junctions)
            content.Rows.push_back(JunctionRow(*context.World, junction));
        NameMatchingJunctions(content.Rows, status->Junctions);
        std::sort(content.Rows.begin(), content.Rows.end(), JunctionBefore);
    }
    EditorUI::SplineJunctionList::Handlers handlers;
    handlers.Select = context.SelectEntity;
    handlers.Act = [context, rows = content.Rows](uint32 nodeId) { JoinAtGrade(context, rows, nodeId); };
    UIElement* section = AddListSection(context, "SplinePaving/Junctions", "Junctions",
                                        "Where roads meet this spline, and whether the paving joins them");
    InspectorUI::AddControl(section, std::make_unique<EditorUI::SplineJunctionList>(content, handlers));
}

// True when `entity` is an Area network member: its paving owns the ground under it.
bool IsArea(const ECS::World& world, ECS::EntityHandle entity)
{
    const auto* member = world.GetComponent<Components::SplineNetworkMember>(entity);
    return member && member->Kind == Components::SplineNetworkMemberKind::Area;
}

// `context` with its rows going into a new section of the paving's inspector.
InspectorContext InSection(const InspectorContext& context, std::string_view key, std::string_view title)
{
    InspectorContext rows = context;
    rows.Parent = InspectorUI::AddComponentSection(context.Parent, key, title)->GetContentContainer();
    return rows;
}

// The note on the ground under a paving on a Road, whose extrude owns that ground.
void AddGroundNote(const InspectorContext& ctx)
{
    InspectorUI::AddInfoCard(ctx.Parent, "The paving lies on the terrain as it is and does not reshape it. Where a "
                                         "road's Ground Authority flattens the terrain under the paving, the terrain "
                                         "can show through the paving. A road that owns its ground clears the grass under it; "
                                         "under a road that only grades the ground, the grass still grows.");
}

// The caption under Seed: what a new seed changes on screen.
void AddSeedCaption(const InspectorContext& context)
{
    InspectorUI::AddTextBlock(InspectorUI::AddActionRow(context.Parent, "inspector-caption-row"),
                              "Offsets the paving pattern on the ground, so two pavings with one material do not "
                              "look alike.",
                              "inspector-field-caption");
}

// The caption under an area's Seam rows: a road with no paving of its own takes the area's Seam settings.
void AddAreaSeamCaption(const InspectorContext& context)
{
    InspectorUI::AddTextBlock(InspectorUI::AddActionRow(context.Parent, "inspector-caption-row"),
                              "These Seam settings apply to each road that enters this paving and has no Spline Paving of its own.",
                              "inspector-field-caption");
}

// The settings in the order a user dresses a square: the surface, the curb, where roads enter, and
// what the paving lies on. The lists and the shadows follow them. `status` is null before the first rebuild.
void AddSettingSections(const InspectorContext& context, const SplinePaving& shown, const PavingStatus* status)
{
    const ECS::ComponentTypeId typeId = ECS::GetComponentTypeId<SplinePaving>();
    const InspectorContext surface = InSection(context, "SplinePaving/Surface", "Surface");
    RenderReflectedFieldRows(surface, typeId, kSurfaceFields);
    AddSeedCaption(surface);

    const InspectorContext curb = InSection(context, "SplinePaving/Curb", "Curb");
    AddPavingBorderSizeRows(curb, shown);
    AddPavingCornerRow(curb, shown);
    RenderReflectedFieldRows(curb, typeId, kCurbMaterialField);
    ShowAssetFieldEmptyAs(*curb.Parent, kCurbMaterialEmptyText);

    const InspectorContext mouths = InSection(context, "SplinePaving/RoadMouths", "Road Mouths");
    AddPavingMouthRow(mouths, shown);
    AddPavingMouthRadiusRow(mouths, shown);
    const bool area = IsArea(*context.World, context.Entity);
    AddPavingSeamRows(mouths, shown, status, area);
    if (area)
        AddAreaSeamCaption(mouths);

    const InspectorContext placement = InSection(context, "SplinePaving/Placement", "Placement");
    if (IsArea(*context.World, context.Entity))
        AddPavingGroundAuthorityRow(placement, shown);
    else
        AddGroundNote(placement);
    AddPavingConformRow(placement, shown);
}

// The last section: the shadows.
void AddShadowSection(const InspectorContext& context)
{
    RenderReflectedFieldRows(InSection(context, "SplinePaving/Shadows", "Shadows"),
                             ECS::GetComponentTypeId<SplinePaving>(), kShadowFields);
}

// The header badge counts the roads whose heights keep them from the paving.
void ShowRoadsNotPaved(const InspectorContext& ctx, const PavingStatus* status)
{
    if (!ctx.Section)
        return;
    const auto unpaved = status ? std::count_if(status->Junctions.begin(), status->Junctions.end(),
                                                [](const PavingJunctionStatus& junction) {
                                                    return junction.State == PavingJunctionState::HeightsDiffer;
                                                })
                                : 0;
    if (unpaved == 0)
    {
        ctx.Section->SetHeaderBadge(std::string());
        return;
    }
    ctx.Section->SetHeaderBadge(std::to_string(unpaved) + (unpaved == 1 ? " road not paved" : " roads not paved"),
                                "The heights differ where they meet: the notice states the fix");
}

// The status the inspector shows, and the request that rebuilds it.
struct PavingStatusFollower
{
    ECS::EntityHandle Entity{};
    std::optional<PavingStatus> Shown;
    std::function<void()> RequestRefresh;
    bool RefreshRequested = false;
};

// The recipe system replaces the status at each rebuild. A status that differs from the shown one
// rebuilds the inspector, so the rows, the junctions and the badge show the same rebuild.
void FollowPavingStatus(PavingStatusFollower& follower)
{
    if (follower.RefreshRequested)
        return;
    const PavingStatus* current = RunningPavingStatus(follower.Entity);
    const bool same = current ? follower.Shown && *follower.Shown == *current : !follower.Shown;
    if (same)
        return;
    follower.RefreshRequested = true;
    follower.RequestRefresh();
}

void AddPavingStatusFollower(const InspectorContext& context, const PavingStatus* status)
{
    if (!context.SimulationRefreshCallbacks || !context.RequestInspectorRefresh)
        return;
    PavingStatusFollower follower{context.Entity, status ? std::optional<PavingStatus>(*status) : std::nullopt,
                                  context.RequestInspectorRefresh};
    context.SimulationRefreshCallbacks->push_back([follower]() mutable { FollowPavingStatus(follower); });
}

void BuildSplinePavingInspector(const InspectorContext& context)
{
    if (!context.World || !context.Parent || !context.Entity.IsValid())
        return;
    AddSplinePavingRows(context, RunningPavingStatus(context.Entity));
}

} // namespace

std::string PavingJunctionStateWords(const ECS::World& world, const PavingJunctionStatus& junction)
{
    switch (junction.State)
    {
    case PavingJunctionState::Paved:
        return "paved";
    case PavingJunctionState::PavedByOther:
        return "paved by " + Editor::EntityDisplayName(world, junction.PavedBy);
    case PavingJunctionState::HeightsDiffer:
        return FormatMeters(std::abs(junction.RoadAboveMeters), kHeightDecimals) +
               (junction.RoadAboveMeters < 0.0f ? " below the paving" : " above the paving");
    case PavingJunctionState::Ignored:
        return "not paved: own surface";
    case PavingJunctionState::CutOut:
        return "not paved: cut out";
    case PavingJunctionState::TooNarrow:
        break;
    }
    return "not paved: too narrow";
}

void AddSplinePavingRows(const InspectorContext& context, const PavingStatus* status)
{
    const auto* recipe = context.World ? context.World->GetComponent<SplinePaving>(context.Entity) : nullptr;
    if (!recipe || !context.Parent)
        return;
    const SplinePaving shown = *recipe;
    const std::vector<std::string> notices = status ? status->Notices : std::vector<std::string>{};
    AddBuildStateNotice(context, SplineRecipes::StateOf(*context.World, context.Entity).State,
                        SplineRecipes::ReasonOf(*context.World, context.Entity), notices);
    ShowRoadsNotPaved(context, status);
    AddSettingSections(context, shown, status);
    AddMemberList(context, shown, status);
    AddJunctionList(context, status);
    AddShadowSection(context);
    AddPavingStatusFollower(context, status);
}

void RegisterSplinePavingInspector()
{
    InspectorRegistry::Get().RegisterComponentInspector<SplinePaving>(BuildSplinePavingInspector);
}

} // namespace GameEngine
