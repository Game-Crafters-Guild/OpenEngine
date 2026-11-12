#include "Rendering/Core/DrawProducerRegistry.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <mutex>

namespace GameEngine::Rendering
{

namespace
{

DrawProducerRegistry::ScopeName MakeScopeName(std::string_view name)
{
    DrawProducerRegistry::ScopeName field{};
    std::memcpy(field.data(), name.data(), std::min(name.size(), field.size() - 1));
    return field;
}

struct RegistryState
{
    std::mutex Mutex;
    size_t Count = 2;
    DrawProducerRegistry::ScopeNameTable Names;

    RegistryState()
    {
        Names.fill(MakeScopeName("Other"));
        Names[DrawProducerRegistry::kInstances] = MakeScopeName("Instances");
    }
};

// Names are written under the mutex before their id is returned; a draw carries an id only
// after its producer registered, so the frame path reads the table without the lock.
RegistryState& State()
{
    static RegistryState state;
    return state;
}

} // namespace

DrawProducerId DrawProducerRegistry::Register(std::string_view scopeName)
{
    assert(!scopeName.empty() && scopeName.size() < kNameCapacity &&
           scopeName.find('.') == std::string_view::npos &&
           "DrawProducerRegistry::Register: a scope name is 1 to 31 characters with no dot");
    RegistryState& state = State();
    std::lock_guard lock(state.Mutex);
    for (size_t id = 0; id < state.Count; ++id)
    {
        if (std::string_view(state.Names[id].data()) == scopeName)
            return static_cast<DrawProducerId>(id);
    }
    assert(state.Count < kCapacity && "DrawProducerRegistry::Register: more than 255 draw producers");
    if (state.Count >= kCapacity)
        return kOther;
    state.Names[state.Count] = MakeScopeName(scopeName);
    return static_cast<DrawProducerId>(state.Count++);
}

const DrawProducerRegistry::ScopeNameTable& DrawProducerRegistry::ScopeNames()
{
    return State().Names;
}

} // namespace GameEngine::Rendering
