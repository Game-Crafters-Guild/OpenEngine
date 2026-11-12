#include "Inspectors/SplineRecipe/Paving/SplinePavingSettingRows.h"

#include "InspectorRegistry.h"
#include "Inspectors/DefaultComponentInspector.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorComponent/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorOverrideRows.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Inspectors/Spline/SplineInspector.h"
#include "UI/Controls/EnumField.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/InspectorNotice.h"
#include "UI/Controls/PictureChoiceField.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Components/Spline/SplinePaving.h"
#include "Core/Engine.h"
#include "ECS/Component/ComponentFieldRegistry.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "SplineRecipes/Paving/PavingStatus.h"

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
namespace
{

using Components::SplinePaving;

template <typename TEnum>
struct PavingChoice
{
    TEnum Value;
    const char* Label;
    const char* Description;
    const char* PictureClass;
};

// The tiles in order: a tile's index is its index here.
constexpr PavingChoice<Components::SplineWallCorner> kCornerChoices[] = {
    {Components::SplineWallCorner::Miter, "Miter", "A sharp corner.", "option-picture--paving-corner-miter"},
    {Components::SplineWallCorner::Round, "Round", "A rounded corner.", "option-picture--paving-corner-round"},
};

constexpr PavingChoice<Components::SplinePavingMouth> kMouthChoices[] = {
    {Components::SplinePavingMouth::Flared, "Flared", "Curves into the road.", "option-picture--paving-mouth-flared"},
    {Components::SplinePavingMouth::Straight, "Straight", "A square corner at the road.",
     "option-picture--paving-mouth-straight"},
};

constexpr PavingChoice<Components::SplineConformTarget> kConformChoices[] = {
    {Components::SplineConformTarget::TerrainOnly, "Terrain Only", "Lies on the terrain only.",
     "option-picture--paving-conform-terrain-only"},
    {Components::SplineConformTarget::Scene, "Scene", "Lies on decks and props.",
     "option-picture--paving-conform-scene"},
};

// The dropdown names what Auto resolves to (Components::ResolveGroundAuthority), as the terrain
// inspector's Auto rows do.
constexpr EnumEntry<Components::SplineGroundAuthority> kGroundAuthorities[] = {
    {Components::SplineGroundAuthority::Auto, "Auto (Owns)"},
    {Components::SplineGroundAuthority::Owns, "Owns"},
    {Components::SplineGroundAuthority::Grades, "Grades"},
    {Components::SplineGroundAuthority::None, "None"},
};
constexpr const char* kGroundAuthorityField = "GroundAuthority";

constexpr PavingChoice<Components::SplinePavingSeam> kSeamChoices[] = {
    {Components::SplinePavingSeam::HardCut, "Hard Cut", "The road's pattern stops on the seam.",
     "option-picture--paving-seam-hard-cut"},
    {Components::SplinePavingSeam::Blend, "Blend", "Bends or fades into the other pattern.",
     "option-picture--paving-seam-blend"},
    {Components::SplinePavingSeam::JointLine, "Joint Line", "A thin line along the seam.",
     "option-picture--paving-seam-joint-line"},
    {Components::SplinePavingSeam::StoneCourse, "Stone Course", "A row of stones along the seam.",
     "option-picture--paving-seam-stone-course"},
};
// The Blend notice names the material that draws the seams: an area's own, or on a road the material of
// the paving it gives way to.
constexpr const char* kAreaSeamMaterialSubject = "this paving's material";
constexpr const char* kRoadSeamMaterialSubject = "the paving it gives way to";
constexpr const char* kBlendUnbuiltText = "No seam uses this setting yet.";
constexpr const char* kBlendMixedText = "The seams this setting reaches meet materials with different Pattern "
                                        "Symmetry: each bends or crossfades by its own.";
constexpr const char* kOpenMaterialLabel = "Open Material";
constexpr const char* kOpenMaterialTooltip = "Opens the material that draws these seams.";
constexpr std::string_view kSeamCourseMaterialField[] = {"SeamCourseMaterial"};
constexpr const char* kSeamBandReason = "Only a Blend seam has a band. Choose Blend to set how far it reaches into the road.";
constexpr const char* kSeamCourseReason = "Only a Joint Line or a Stone Course has a width and a material. Choose one "
                                          "to set them.";
constexpr const char* kCourseEmptyText = "Paving";
constexpr const char* kJointLineEmptyText = "Paving, darker";
constexpr const char* kStoneCourseEmptyText = "Paving, as stones";
constexpr const char* kJointLineEmptyTooltip = "Empty: the paving's material draws the joint, darker.";
constexpr const char* kStoneCourseEmptyTooltip =
    "Empty: the paving's material draws the course, its stones turned across the seam and a little brighter.";
constexpr const char* kOwnsNote = "Grades the ground to a smooth surface that follows the rim and meets each road at its "
                                  "height. Clears its grass, paints it Dirt, and claims it, so other grading stops at its "
                                  "edge. Planting beds cut out of the paving are graded and painted too.";

constexpr const char* kUndoPrefix = "Change Spline Paving ";
// The curb's width and height reach 1 m on the slider; a typed value takes the field's whole range.
constexpr float kCurbSliderMaxMeters = 1.0f;
constexpr const char* kMouthRadiusStraightReason = "A Straight mouth has no curve. Choose Flared to set its radius.";

// A Course Width of 0 reads "Auto" in the 60 px number box; the box's tooltip says what it draws.
constexpr const char* kAutoWidthText = "Auto";
constexpr const char* kJointLineAutoWidthTooltip = "Auto: a 3 cm joint. Type a width in meters to set it.";
constexpr const char* kStoneCourseAutoWidthTooltip =
    "Auto: a row of stones half a tile of the other pattern wide. Type a width in meters to set it.";

// What an empty Course Material draws: only a seam that draws a course names its look.
const char* CourseMaterialEmptyText(Components::SplinePavingSeam seam)
{
    if (seam == Components::SplinePavingSeam::JointLine)
        return kJointLineEmptyText;
    if (seam == Components::SplinePavingSeam::StoneCourse)
        return kStoneCourseEmptyText;
    return kCourseEmptyText;
}

// The Blend notice's text: the form the last rebuild found from the pattern symmetry of the material that
// draws the seams (SplineRecipes::PavingStatus::SeamPatternSymmetry). `area` selects the subject.
std::string BlendNoticeText(const SplineRecipes::PavingStatus* status, bool area)
{
    if (status && status->SeamPatternMixed)
        return kBlendMixedText;
    if (!status || !status->SeamPatternSymmetry)
        return kBlendUnbuiltText;
    const std::string subject = area ? kAreaSeamMaterialSubject : kRoadSeamMaterialSubject;
    switch (*status->SeamPatternSymmetry)
    {
    case MaterialPatternSymmetry::Square:
        return "Bending: " + subject + " has a Square pattern.";
    case MaterialPatternSymmetry::Rectangular:
        return "Bending where the patterns line up: " + subject + " has a Rectangular pattern.";
    case MaterialPatternSymmetry::None:
        break;
    }
    return "Crossfade: " + subject + " has no Pattern Symmetry. Set Pattern Symmetry in the material to bend.";
}

// The file of the material that draws the seams (SplineRecipes::PavingStatus::SeamMaterialPaving), or
// empty when no one paving draws them, its Material is empty, or the registry does not know it.
std::filesystem::path SeamMaterialPath(const ECS::World& world, const SplineRecipes::PavingStatus* status)
{
    if (!status || !status->SeamMaterialPaving.IsValid() || !world.IsValid(status->SeamMaterialPaving))
        return {};
    const auto* paving = world.GetComponent<SplinePaving>(status->SeamMaterialPaving);
    if (!paving || paving->Material.IsNull())
        return {};
    AssetManager& assets = EngineCore::GetInstance().GetAssetManager();
    AssetMetadata metadata;
    if (!assets.GetRegistry().TryGetAssetMetadata(paving->Material.ToGuid(), metadata) || metadata.Path.empty())
        return {};
    return assets.ResolveAssetPath(metadata.Path);
}

// The Information notice under the Seam tiles while Blend is chosen: the form the seams take, and the
// action that opens the material that draws them when one paving's material does.
void AddBlendNotice(const InspectorContext& context, const SplineRecipes::PavingStatus* status, bool area)
{
    EditorUI::InspectorNotice* notice = InspectorUI::AddNotice(context.Parent, BlendNoticeText(status, area),
                                                               EditorUI::InspectorNotice::Kind::Information);
    if (!notice || !context.OpenAsset)
        return;
    std::filesystem::path material = SeamMaterialPath(*context.World, status);
    if (material.empty())
        return;
    notice->SetAction(kOpenMaterialLabel, kOpenMaterialTooltip,
                      [open = context.OpenAsset, material = std::move(material)] { open(material); });
}

// The tiles of `choices`, in order.
template <typename TEnum>
std::vector<EditorUI::PictureChoiceField::Choice> TilesOf(std::span<const PavingChoice<TEnum>> choices)
{
    std::vector<EditorUI::PictureChoiceField::Choice> tiles;
    for (const PavingChoice<TEnum>& choice : choices)
        tiles.push_back({choice.Label, choice.Description, choice.PictureClass});
    return tiles;
}

template <typename TEnum>
size_t ChoiceIndex(std::span<const PavingChoice<TEnum>> choices, TEnum value)
{
    for (size_t i = 0; i < choices.size(); ++i)
    {
        if (choices[i].Value == value)
            return i;
    }
    return 0;
}

// Writes the choice at `index` into the field `member` of every selected paving, one undo step,
// and rebuilds the inspector so the rows that depend on the field follow it.
template <typename TEnum>
void CommitChoice(const InspectorContext& context, std::span<const PavingChoice<TEnum>> choices,
                  TEnum SplinePaving::*member, const std::string& undoName, size_t index)
{
    if (!context.World || index >= choices.size())
        return;
    const TEnum value = choices[index].Value;
    InspectorDrag::CommitComponentsWithUndo<SplinePaving>(
        context.World, context.Entity, InspectorDrag::GetAdditionalEntities(context), context.ChangeNotifications,
        context.Undo, undoName, [member, value](SplinePaving& paving) { paving.*member = value; });
    if (context.RequestInspectorRefresh)
        context.RequestInspectorRefresh();
}

// The row label of the reflected field `fieldName`, as the reflected rows show it.
std::string FieldLabel(const ECS::FieldInfo* field, std::string_view fieldName)
{
    return field ? FieldDisplayName(*field) : std::string(fieldName);
}

// The row of the reflected field `fieldName`: its label and tooltip from the reflection, `tiles` (one
// per entry of `choices`), and the override mark of a single selection.
template <typename TEnum>
void AddChoiceRow(const InspectorContext& context, std::string_view fieldName,
                  std::span<const PavingChoice<TEnum>> choices, std::vector<EditorUI::PictureChoiceField::Choice> tiles,
                  TEnum SplinePaving::*member, const SplinePaving& shown)
{
    if (!context.Parent || !context.World)
        return;
    const ECS::ComponentTypeId typeId = ECS::GetComponentTypeId<SplinePaving>();
    const ECS::FieldInfo* field = ECS::ComponentFieldRegistry::FindField(typeId, fieldName);
    const std::string label = FieldLabel(field, fieldName);
    auto picker = std::make_unique<EditorUI::PictureChoiceField>(std::move(tiles), ChoiceIndex(choices, shown.*member));
    const std::string undoName = kUndoPrefix + label;
    picker->SetOnSelected([context, choices, member, undoName](size_t index)
                          { CommitChoice(context, choices, member, undoName, index); });
    auto* added = InspectorUI::AddPictureChoiceRow(context.Parent, label, std::move(picker),
                                                   InspectorDrag::ReflectedTooltip(typeId, fieldName));
    BindFieldRow(context, typeId, InspectorUI::RowOfControl(added), std::string(fieldName));
}

// The dropdown row of the reflected enum field `fieldName`: its label and tooltip from the reflection,
// the override mark of a single selection, and a pick written to `member` of every selected paving as
// one undo step. The inspector rebuilds after a pick, so the rows that depend on the field follow it.
template <typename TEnum, size_t Count>
void AddEnumChoiceRow(const InspectorContext& context, const char* fieldName, const EnumEntry<TEnum> (&entries)[Count],
                      TEnum SplinePaving::*member, const SplinePaving& shown)
{
    const ECS::ComponentTypeId typeId = ECS::GetComponentTypeId<SplinePaving>();
    const std::string label = FieldLabel(ECS::ComponentFieldRegistry::FindField(typeId, fieldName), fieldName);
    auto* field = InspectorUI::AddEnumRow(context.Parent, label, entries, shown.*member,
                                          InspectorDrag::ReflectedTooltip(typeId, fieldName));
    const std::string undoName = kUndoPrefix + label;
    field->SetOnValueChanged(
        [context, undoName, member](TEnum value)
        {
            InspectorDrag::CommitComponentsWithUndo<SplinePaving>(
                context.World, context.Entity, InspectorDrag::GetAdditionalEntities(context),
                context.ChangeNotifications, context.Undo, undoName,
                [member, value](SplinePaving& paving) { paving.*member = value; });
            if (context.RequestInspectorRefresh)
                context.RequestInspectorRefresh();
        });
    BindFieldRow(context, typeId, InspectorUI::RowOfControl(field), fieldName);
}

bool DrawsCourse(Components::SplinePavingSeam seam)
{
    return seam == Components::SplinePavingSeam::JointLine || seam == Components::SplinePavingSeam::StoneCourse;
}

} // namespace

void AddPavingCornerRow(const InspectorContext& context, const SplinePaving& shown)
{
    AddChoiceRow<Components::SplineWallCorner>(context, "BorderCorner", kCornerChoices,
                                               TilesOf<Components::SplineWallCorner>(kCornerChoices),
                                               &SplinePaving::BorderCorner, shown);
}

void AddPavingMouthRow(const InspectorContext& context, const SplinePaving& shown)
{
    AddChoiceRow<Components::SplinePavingMouth>(context, "Mouth", kMouthChoices,
                                                TilesOf<Components::SplinePavingMouth>(kMouthChoices),
                                                &SplinePaving::Mouth, shown);
}

void AddPavingConformRow(const InspectorContext& context, const SplinePaving& shown)
{
    AddChoiceRow<Components::SplineConformTarget>(context, "ConformTarget", kConformChoices,
                                                  TilesOf<Components::SplineConformTarget>(kConformChoices),
                                                  &SplinePaving::ConformTarget, shown);
}

void AddPavingGroundAuthorityRow(const InspectorContext& context, const SplinePaving& shown)
{
    if (!context.Parent || !context.World)
        return;
    AddEnumChoiceRow(context, kGroundAuthorityField, kGroundAuthorities, &SplinePaving::GroundAuthority, shown);
    if (Components::ResolveGroundAuthority(shown) == Components::SplineGroundAuthority::Owns)
        InspectorUI::AddInfoCard(context.Parent, kOwnsNote);
}

void AddPavingBorderSizeRows(const InspectorContext& context, const SplinePaving& shown)
{
    InspectorDrag::AddReflectedLengthSliderRow(context, "BorderWidth", &SplinePaving::BorderWidth, shown, kUndoPrefix,
                                               kCurbSliderMaxMeters);
    InspectorDrag::AddReflectedLengthSliderRow(context, "BorderHeight", &SplinePaving::BorderHeight, shown,
                                               kUndoPrefix, kCurbSliderMaxMeters);
}

void AddPavingMouthRadiusRow(const InspectorContext& context, const SplinePaving& shown)
{
    UIElement* row =
        InspectorDrag::AddReflectedLengthSliderRow(context, "MouthRadius", &SplinePaving::MouthRadius, shown,
                                                   kUndoPrefix);
    if (row && shown.Mouth == Components::SplinePavingMouth::Straight)
        InspectorUI::DisableRow(row, kMouthRadiusStraightReason);
}

void AddPavingSeamRows(const InspectorContext& context, const SplinePaving& shown,
                       const SplineRecipes::PavingStatus* status, bool area)
{
    if (!context.Parent || !context.World)
        return;
    AddChoiceRow<Components::SplinePavingSeam>(context, "Seam", kSeamChoices,
                                               TilesOf<Components::SplinePavingSeam>(kSeamChoices),
                                               &SplinePaving::Seam, shown);
    if (shown.Seam == Components::SplinePavingSeam::Blend)
        AddBlendNotice(context, status, area);
    UIElement* band = InspectorDrag::AddReflectedLengthSliderRow(context, "SeamBandTiles", &SplinePaving::SeamBandTiles,
                                                                 shown, kUndoPrefix);
    if (band && shown.Seam != Components::SplinePavingSeam::Blend)
        InspectorUI::DisableRow(band, kSeamBandReason);
    const bool drawsCourse = DrawsCourse(shown.Seam);
    const bool jointLine = shown.Seam == Components::SplinePavingSeam::JointLine;
    UIElement* width = InspectorDrag::AddReflectedLengthSliderRow(context, "SeamCourseWidth",
                                                                  &SplinePaving::SeamCourseWidth, shown, kUndoPrefix);
    if (width && !drawsCourse)
        InspectorUI::DisableRow(width, kSeamCourseReason);
    FloatField* widthField = InspectorDrag::FindFloatField(width);
    if (widthField && drawsCourse)
    {
        widthField->SetNoneValue(0.0f, kAutoWidthText);
        widthField->SetTooltip(jointLine ? kJointLineAutoWidthTooltip : kStoneCourseAutoWidthTooltip);
    }
    RenderReflectedFieldRows(context, ECS::GetComponentTypeId<SplinePaving>(), kSeamCourseMaterialField);
    UIElement* material = ShowAssetFieldEmptyAs(*context.Parent, CourseMaterialEmptyText(shown.Seam));
    if (material && !drawsCourse)
        InspectorUI::DisableRow(material, kSeamCourseReason);
    else if (material)
        material->SetTooltip(jointLine ? kJointLineEmptyTooltip : kStoneCourseEmptyTooltip);
}

} // namespace GameEngine
