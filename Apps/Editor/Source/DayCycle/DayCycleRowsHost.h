#pragma once

#include "DayCycle/DayCycleEditorServices.h"
#include "DayCycle/DayCycleTrackRows.h"
#include "UI/UIElement.h"
#include "UI/UIManagerRef.h"

#include <cstdint>
#include <memory>

namespace GameEngine::Editor
{

/// The element the profile inspector's track rows live in. It owns the rows, so they follow the
/// profile exactly as long as their UI is shown (an asset inspector gets no refresh list from the
/// inspector panel), and it moves their playhead to the sun now on its UI manager's periodic refresh.
class DayCycleRowsHost final : public UIElement
{
  public:
    DayCycleRowsHost(DayCycleTrackRowsContext context, DayCycleEditorServices services);
    ~DayCycleRowsHost() override;

    /// Moves the rows' playhead to the sun now, and builds what changed since the last call.
    void Refresh();

    void OnOwnerManagerChanged(UIManager* owner) override;

  private:
    DayCycleEditorServices m_Services;
    std::unique_ptr<DayCycleTrackRows> m_Rows;
    UIManagerRef m_RefreshManager;
    std::uint64_t m_RefreshToken = 0;
};

} // namespace GameEngine::Editor
