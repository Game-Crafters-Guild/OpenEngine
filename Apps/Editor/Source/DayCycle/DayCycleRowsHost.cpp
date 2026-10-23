#include "DayCycle/DayCycleRowsHost.h"

#include "DayCycle/DayCycleSunNow.h"
#include "UI/UIManager.h"

#include <utility>

namespace GameEngine::Editor
{

DayCycleRowsHost::DayCycleRowsHost(DayCycleTrackRowsContext context, DayCycleEditorServices services)
    : m_Services(std::move(services))
{
    AddClass("day-cycle-rows-host");
    m_Rows = std::make_unique<DayCycleTrackRows>(*this, std::move(context));
    Refresh();
}

DayCycleRowsHost::~DayCycleRowsHost()
{
    if (UIManager* manager = m_RefreshManager.Get(); manager && m_RefreshToken != 0)
        manager->UnregisterPeriodicRefresh(m_RefreshToken);
    m_Rows.reset();
}

void DayCycleRowsHost::OnOwnerManagerChanged(UIManager* owner)
{
    UIElement::OnOwnerManagerChanged(owner);
    if (UIManager* previous = m_RefreshManager.Get(); previous && m_RefreshToken != 0)
        previous->UnregisterPeriodicRefresh(m_RefreshToken);
    m_RefreshToken = 0;
    m_RefreshManager = UIManagerRef(owner);
    if (owner)
        m_RefreshToken = owner->RegisterPeriodicRefresh([this]() { Refresh(); });
}

void DayCycleRowsHost::Refresh()
{
    ECS::World* world = m_Services.World ? m_Services.World() : nullptr;
    if (!world || !m_Rows)
        return;
    const DayCycleSunNow now = ReadDayCycleSunNow(*world, m_Services.Render ? m_Services.Render() : nullptr,
                                                  m_Services.Sky ? m_Services.Sky() : nullptr);
    m_Rows->SetPlayhead(now.Hours, now.ElevationDegrees, now.DuskHalf, now.HourPlacesSun);
}

} // namespace GameEngine::Editor
