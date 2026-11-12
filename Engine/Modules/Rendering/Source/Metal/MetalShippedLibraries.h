#pragma once

#include "Rendering/ShaderCache/MetalLibraryArchive.h"

#include "Types/FlatMap.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>

namespace MTL
{
class Device;
class Function;
} // namespace MTL

namespace GameEngine::Rendering
{

struct MetalShaderTranslation;

// The stage libraries an export compiled offline (MetalLibraryArchive), found by translation key. The first
// lookup maps the archive and reads only its index; a library's pages load when its stage is looked up. The
// mapping stays until the archive path changes. Thread-safe: pipeline builds look up from job workers.
class MetalShippedLibraries
{
public:
    // The main0 function of the shipped library for @p key, with the stage's workgroup size in
    // @p outTranslation. Nullptr when @p archive is empty, cannot be read, was built under other
    // options than @p optionsHash, or holds no library for @p key.
    MTL::Function* NewFunction(MTL::Device* device, const std::filesystem::path& archive, uint64_t optionsHash,
                               uint64_t key, MetalShaderTranslation& outTranslation);

private:
    // A read-only mapping of the archive file. The entries point into it.
    struct Loaded
    {
        Loaded() = default;
        Loaded(const Loaded&) = delete;
        Loaded& operator=(const Loaded&) = delete;
        ~Loaded();

        void* Mapping = nullptr;
        size_t Size = 0;
        FlatMap<uint64_t, MetalLibraryArchiveEntry> Entries;
    };

    std::shared_ptr<const Loaded> Acquire(const std::filesystem::path& archive, uint64_t optionsHash);

    std::mutex m_Mutex;
    std::filesystem::path m_Archive;
    std::shared_ptr<const Loaded> m_Loaded;
};

} // namespace GameEngine::Rendering
