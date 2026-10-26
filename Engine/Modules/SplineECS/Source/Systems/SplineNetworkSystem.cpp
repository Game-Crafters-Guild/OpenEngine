#include "SplineECS/Systems/SplineNetworkSystem.h"
#include "SplineECS/SplineNetwork.h"

namespace GameEngine::SplineECS
{

void SplineNetworkSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    if (SplineNetwork* network = SplineNetwork::TryGet())
        network->Update(world);
}

} // namespace GameEngine::SplineECS
