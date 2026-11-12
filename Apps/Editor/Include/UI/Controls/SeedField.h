#pragma once

#include "Types/Types.h"
#include "UI/UIElement.h"

#include <functional>

namespace GameEngine
{
class IntField;
}

namespace GameEngine::EditorUI
{

/// A seed from 0 to 2,147,483,647: an integer field, then a dice button at its right end that picks a
/// new seed. A typed seed and a dice click each commit through the one handler. A layout declares it
/// as <SeedField/>. The look comes from UI/controls/SeedField/SeedField.css.
class SeedField final : public UIElement
{
  public:
    using CommitHandler = std::function<void(uint64)>;

    SeedField();

    /// Shows `seed` and commits nothing. A seed above the field's range shows as the largest seed.
    void SetSeed(uint64 seed);

    /// Sets the call for a seed the user commits: a typed seed or a dice click.
    void SetOnSeedCommitted(CommitHandler handler);

  private:
    void Commit(uint64 seed);
    void CommitNewSeed();

    CommitHandler m_OnSeedCommitted;
    IntField* m_Input = nullptr;
};

} // namespace GameEngine::EditorUI
