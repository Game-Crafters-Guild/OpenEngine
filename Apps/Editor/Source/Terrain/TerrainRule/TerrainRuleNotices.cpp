#include "Terrain/TerrainRule/TerrainRuleNotices.h"

#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainSurfaceRules.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Terrain/TerrainRule/TerrainRuleConditionVocabulary.h"
#include "TerrainECS/Scene/TerrainSceneSchemas.h"
#include "UI/Controls/InspectorNotice.h"

#include <string>

namespace GameEngine::Editor
{
namespace
{

// Every rules notice is a problem of the rows: the warning look.
constexpr EditorUI::InspectorNotice::Kind kWarning = EditorUI::InspectorNotice::Kind::Warning;

} // namespace

void AddSurfaceRulesEmptyNotice(UIElement* parent, uint32 ruleCount)
{
    if (!parent || ruleCount > 0)
        return;

    InspectorUI::AddNotice(parent,
        "No rules yet, so this effect writes nothing. Add Rule below, then give the rule a condition "
        "to say where it applies, or pick a rule set from the Replace rules list under Add Rule.", kWarning);
}

void AddSurfaceRulesPlanetNotice(UIElement* parent, Components::TerrainDomain domain)
{
    if (!parent || domain != Components::TerrainDomain::Spherical)
        return;

    InspectorUI::AddNotice(parent,
        "Surface rules do not paint a planet. The rows are saved with the scene and do nothing here. "
        "Shape a planet's ground with Noise, Flatten, Stamp or Sculpt zones.", kWarning);
}

void AddRuleRowStateNotice(UIElement* parent, const Components::TerrainSurfaceRule& rule,
                           uint32 ruleIndex)
{
    if (!parent)
        return;

    const std::string inert = TerrainRuleVocabulary::RuleInertReason(rule);
    if (!inert.empty())
    {
        InspectorUI::AddNotice(parent, inert, kWarning);
        return;
    }

    // Not a warning: an unconditional row is a legitimate authoring choice (a
    // base material under everything else is exactly this), it just covers more
    // than a reader of the row alone would expect.
    if (rule.ConditionCount == 0)
        InspectorUI::AddInfoCard(parent,
                                 std::string(TerrainRuleVocabulary::UnconditionalRuleNote()));

    (void)ruleIndex; // The row's own heading already numbers it.
}

std::string RuleCapReason(uint32 ruleCount)
{
    if (ruleCount < Components::kMaxTerrainSurfaceRules)
        return {};
    return "This effect holds the maximum of "
         + std::to_string(Components::kMaxTerrainSurfaceRules)
         + " rules. To add more, split the rule set across a second Surface Rules effect on "
           "another volume - they composite in stack order.";
}

std::string ConditionCapReason(uint32 ruleIndex, uint32 conditionCount)
{
    if (conditionCount < Components::kMaxTerrainRuleConditions)
        return {};
    return "Rule " + std::to_string(ruleIndex + 1) + " holds the maximum of "
         + std::to_string(Components::kMaxTerrainRuleConditions)
         + " conditions. To narrow it further, split it into a second rule targeting the same "
           "material - the two rules' weights add.";
}

void AddRuleCapNotice(UIElement* parent, uint32 ruleCount)
{
    const std::string reason = RuleCapReason(ruleCount);
    if (!parent || reason.empty())
        return;
    InspectorUI::AddNotice(parent, reason, kWarning);
}

void AddConditionCapNotice(UIElement* parent, uint32 ruleIndex, uint32 conditionCount)
{
    const std::string reason = ConditionCapReason(ruleIndex, conditionCount);
    if (!parent || reason.empty())
        return;
    InspectorUI::AddNotice(parent, reason, kWarning);
}

void AddRefusedConditionNotice(UIElement* parent, uint32 ruleIndex, uint32 conditionIndex, uint8 refusedKind)
{
    if (!parent || refusedKind == 0)
        return;
    InspectorUI::AddNotice(parent,
        Scene::RefusedRuleConditionKindMessage(ruleIndex, conditionIndex, refusedKind), kWarning);
}

} // namespace GameEngine::Editor
