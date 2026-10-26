#include "PhysicsECS/MeshColliderData.h"

#include "GenerationalVector/GenerationalVector.h"
#include "Physics/PhysicsShapes.h"

#include <cmath>
#include <mutex>

namespace GameEngine::PhysicsECS::MeshColliderData
{
namespace
{
std::mutex s_Mutex;
GenerationalVector::GenerationalVector<std::shared_ptr<const Physics::MeshShapeDef>> s_Meshes;
}

GenerationalVector::Handle Register(Physics::MeshShapeDef mesh)
{
    if (mesh.vertices.size() < 3 || mesh.indices.empty() || mesh.indices.size() % 3 != 0)
        return {};
    for (const auto& vertex : mesh.vertices)
    {
        if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) || !std::isfinite(vertex.z))
            return {};
    }
    for (const auto index : mesh.indices)
    {
        if (index >= mesh.vertices.size())
            return {};
    }
    auto data = std::make_shared<const Physics::MeshShapeDef>(std::move(mesh));
    std::lock_guard lock(s_Mutex);
    return s_Meshes.Create(std::move(data));
}

std::shared_ptr<const Physics::MeshShapeDef> Read(GenerationalVector::Handle handle)
{
    std::lock_guard lock(s_Mutex);
    const auto* data = s_Meshes.Get(handle);
    return data ? *data : nullptr;
}

void Release(GenerationalVector::Handle handle)
{
    std::lock_guard lock(s_Mutex);
    if (s_Meshes.IsValid(handle))
        s_Meshes.Destroy(handle);
}
}
