#include "ECSModules/Audio/Systems/AudioListenerSystem.h"

#include "Audio/AudioSystem.h"
#include "Components/Audio/AudioListener.h"
#include "Components/Transform.h"
#include "ECS/Query.h"
#include "ECS/World.h"

namespace GameEngine { namespace Engine::Audio {
using namespace ::GameEngine::Audio;

using GameEngine::Components::AudioListener;
using GameEngine::Components::WorldTransform;

void FillListenerStateFromWorldTransform(const WorldTransform& wt, Audio::ListenerState& out)
{
    const Mathematics::Vector3 position = wt.GetPosition() - Mathematics::WorldPosition{};
    out.position[0] = position.x;
    out.position[1] = position.y;
    out.position[2] = position.z;

    out.up[0] = wt.Basis[1].x;
    out.up[1] = wt.Basis[1].y;
    out.up[2] = wt.Basis[1].z;

    // Local +Z is forward (LH). Same world-space vector miniaudio uses for
    // emitter positions, so stereo front matches the camera look.
    out.forward[0] = wt.Basis[2].x;
    out.forward[1] = wt.Basis[2].y;
    out.forward[2] = wt.Basis[2].z;
}

void AudioListenerSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    if (!m_Audio || !m_Audio->IsInitialized())
        return;

    auto q = world.Query<ECS::Read<AudioListener>, ECS::Optional<WorldTransform>,
                         ECS::Optional<Components::Transform>>();
    q.Each([&](ECS::EntityHandle /*e*/, const AudioListener& listener, const WorldTransform* wt,
               const Components::Transform* local)
           {
               // A pose the hierarchy has not resolved yet (Version 0 beside a
               // Transform) would place the listener at the origin for a frame;
               // it keeps its previous state instead.
               if (wt && local && wt->Version == 0u)
                   return;
               Audio::ListenerState s{};
               if (wt)
               {
                   FillListenerStateFromWorldTransform(*wt, s);
               }

               m_Audio->SetListener(static_cast<Audio::AudioWorldId>(listener.worldId),
                                    static_cast<uint32>(listener.listenerIndex),
                                    s);
           });
}

} } // namespace GameEngine::Engine::Audio



