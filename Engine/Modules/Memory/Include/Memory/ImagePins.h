#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace GameEngine::Memory::Detail
{

/// The names of the registered arenas that hold a recorded destructor whose code lies in
/// `[base, base + size)`: the image-pin source the engine's module-image observer asks before
/// it unmaps a superseded game image. An arena whose own storage lies inside the range is
/// skipped, because that image's static destructors destroy it while its code is still mapped.
///
/// Each arena is tested by one interval, from its lowest to its highest recorded destructor
/// address, so the answer can name an arena that holds no destructor in the image (it then
/// keeps the image mapped with a warning, never unmaps code still needed). Takes the registry
/// mutex and allocates the answer: call it at an unload decision, not per frame.
std::vector<std::string> DescribeArenasPinningImage(std::uint64_t base, std::uint64_t size);

} // namespace GameEngine::Memory::Detail
