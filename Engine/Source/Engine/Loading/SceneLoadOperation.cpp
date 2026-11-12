#include "Engine/Loading/SceneLoadOperation.h"

#include "SceneLoadState.h"

#include <utility>

namespace GameEngine
{
namespace
{
// The events of a default-constructed operation, which belongs to no load: nothing raises them.
template <typename... Args>
Event<Args...>& NeverRaised()
{
    static Event<Args...> event;
    return event;
}
} // namespace

SceneLoadOperation::SceneLoadOperation(std::shared_ptr<SceneLoadState> state) : m_State(std::move(state)) {}

float SceneLoadOperation::Progress() const
{
    return m_State ? static_cast<float>(m_State->Progress.Progress()) : 0.0f;
}

bool SceneLoadOperation::IsProgressShown() const
{
    return m_State && m_State->Progress.IsProgressShown();
}

double SceneLoadOperation::EstimatedSecondsRemaining() const
{
    return m_State ? m_State->SecondsRemaining : 0.0;
}

bool SceneLoadOperation::IsReadyToShow() const
{
    return m_State && m_State->ReadyToShow;
}

void SceneLoadOperation::Reveal()
{
    if (m_State)
        m_State->RevealRequested = true;
}

Event<>& SceneLoadOperation::Ready() const
{
    return m_State ? m_State->Ready : NeverRaised<>();
}

Event<>& SceneLoadOperation::Revealed() const
{
    return m_State ? m_State->Revealed : NeverRaised<>();
}

Event<>& SceneLoadOperation::Cancelled() const
{
    return m_State ? m_State->Cancelled : NeverRaised<>();
}

Event<std::string_view>& SceneLoadOperation::Failed() const
{
    return m_State ? m_State->Failed : NeverRaised<std::string_view>();
}
} // namespace GameEngine
