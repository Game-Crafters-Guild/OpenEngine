#include "UI/Controls/SeedField.h"

#include "Inspectors/InspectorDragHelpers.h"
#include "Logger/Logger.h"
#include "Types/Fnv1a.h"
#include "UI/ControlLayoutLoader.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/IntField.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIEvents.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <memory>
#include <utility>

namespace GameEngine::EditorUI
{
namespace
{

constexpr const char* kLayoutAssetPath = "UI/controls/SeedField/SeedField.uxml";
constexpr const char* kStyleAssetPath = "UI/controls/SeedField/SeedField.css";

// The largest seed the field shows.
constexpr uint64 kMaxSeed = INT_MAX;

// A seed in the field's range that differs from `current`, mixed from `current` and the clock.
uint64 PickNewSeed(uint64 current)
{
    const auto ticks = static_cast<uint64>(std::chrono::steady_clock::now().time_since_epoch().count());
    const uint64 picked = Hashing::SplitMix64(current ^ ticks) % (kMaxSeed + 1);
    return picked == current ? (picked + 1) % (kMaxSeed + 1) : picked;
}

} // namespace

SeedField::SeedField()
{
    AddClass("seed-field");
    RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");

    std::unique_ptr<UIElement> group = Editor::ControlLayoutLoader::Load(kLayoutAssetPath, {});
    if (!group)
        return;
    m_Input = dynamic_cast<IntField*>(InspectorDrag::FindChildByClass(group.get(), "seed-field-input"));
    auto* dice = dynamic_cast<Button*>(InspectorDrag::FindChildByClass(group.get(), "seed-field-new"));
    if (!m_Input || !dice)
    {
        Logger::Log::Error("SeedField: {} needs the seed-field-input IntField and the seed-field-new Button.",
                           kLayoutAssetPath);
        m_Input = nullptr;
        return;
    }
    m_Input->SetRange(0, static_cast<int>(kMaxSeed));
    m_Input->SetOnValueChanged([this](const int& value) { Commit(static_cast<uint64>(std::max(value, 0))); });
    dice->SetTooltip("New seed");
    dice->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { CommitNewSeed(); });
    AddChild(std::move(group));
}

void SeedField::SetSeed(uint64 seed)
{
    if (m_Input)
        m_Input->SetValueWithoutNotify(static_cast<int>(std::min(seed, kMaxSeed)));
}

void SeedField::SetOnSeedCommitted(CommitHandler handler)
{
    m_OnSeedCommitted = std::move(handler);
}

void SeedField::Commit(uint64 seed)
{
    if (m_OnSeedCommitted)
        m_OnSeedCommitted(seed);
}

// The field shows the new seed before the commit, so a handler that reads the field sees it.
void SeedField::CommitNewSeed()
{
    if (!m_Input)
        return;
    const uint64 seed = PickNewSeed(static_cast<uint64>(std::max(m_Input->GetValue(), 0)));
    SetSeed(seed);
    Commit(seed);
}

} // namespace GameEngine::EditorUI

namespace RegisterSeedField
{
static auto s_RegisterSeedField = GameEngine::UIRegistration::RegisterWithFactory<GameEngine::EditorUI::SeedField>(
    "SeedField", []() { return std::make_unique<GameEngine::EditorUI::SeedField>(); });
}
