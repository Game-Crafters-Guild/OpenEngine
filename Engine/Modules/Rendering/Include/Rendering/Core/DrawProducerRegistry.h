#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace GameEngine::Rendering
{

// The one-byte id of a draw's producer (DrawCommand::Producer). The cull record keys on it too.
using DrawProducerId = uint8_t;

/// The producers of draws inside shared passes. A feature registers its producer once, at its own
/// registration site, and stamps the id on every draw it emits. The profiler names the producer's
/// scope row ("<pass name>.<scope name>") from the registered name.
class DrawProducerRegistry
{
  public:
    static constexpr DrawProducerId kOther = 0;     // a draw whose producer sets no id
    static constexpr DrawProducerId kInstances = 1; // the entity batches (the Rendering module's own)
    // Every value of DrawProducerId indexes the name table.
    static constexpr size_t kIdCount = size_t{std::numeric_limits<DrawProducerId>::max()} + 1;
    static constexpr size_t kCapacity = kIdCount - 1; // ids 0 to 254
    static constexpr size_t kNameCapacity = 32;       // bytes per name, the NUL included

    using ScopeName = std::array<char, kNameCapacity>;
    using ScopeNameTable = std::array<ScopeName, kIdCount>;

    /// Registers `scopeName` (PascalCase, no dot, at most 31 characters) and returns its id. A name
    /// registered before returns its first id, so a feature that initializes again keeps its id.
    static DrawProducerId Register(std::string_view scopeName);

    /// The scope name of every id, indexed by id; an id never registered reads "Other". Read the
    /// table once per pass, not once per draw.
    static const ScopeNameTable& ScopeNames();
};

} // namespace GameEngine::Rendering
