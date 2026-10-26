#pragma once

#include "Types/Types.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace GameEngine { namespace Rendering {

// Every shader source file the compile path reads, held in memory: read once,
// hashed once, scanned for its #include directives once, and served from memory
// until an edit is announced. Every reader goes through it: the include-closure
// walk that builds the compile key, the includer that feeds shaderc, the
// composer's surface and adapter reads, a file stage's top-level source and the
// cache-hit revalidation. The key and the compile read the same held bytes, and
// a compile during which an edit was announced may have read newer bytes than its
// key folded: CompileProgramToCache compares Generation() from before the key
// with the one after the compile and returns such a program without storing it,
// so a stored program always matches the key it is stored under.
//
// A material prewarm builds thousands of programs whose closures share the same
// hundred engine headers, and the key of each one is built (closure included)
// before the compile cache can be probed, so a cache hit pays for the closure
// too. Held in memory, each header costs one read per process, not one per
// program.
//
// THE CACHE IS ANNOUNCED, NOT VALIDATED. A lookup never touches the disk once a
// file is held, so an edit is seen only after NotifyShaderSourcesChanged
// (HotReloadIntegration.h) runs, which empties the whole cache. The editor's
// file watcher reaches it through MaterialSystem's shader-source lane; every
// in-process writer of a shader source calls it after writing (the generated
// shader-graph surfaces under the cache root, which no watcher reports); a
// process with no watcher (a cook, a player) reads each file once for its
// lifetime. A test that rewrites a shader source announces the edit.
//
// Where a directive resolves is cached on the same terms: the first file that
// exists along the search order is remembered, and a file appearing or
// disappearing earlier in that order is seen after the next announcement.
//
// Nothing is evicted between announcements. Entries are bounded by the distinct
// files the process reads (each held once, whatever number of programs reach
// it); resolutions by the distinct search-root lists times the (directory,
// operand) pairs walked under each.
class ShaderSourceCache
{
public:
    struct Entry
    {
        std::string Bytes;
        uint64_t Hash = 0;
        // Operands of this file's own #include directives, so a held file costs
        // no re-scan of its text.
        std::vector<std::string> IncludeOperands;
    };

    // Shared, never copied: one engine header is reached from dozens of
    // shaders. A handle keeps its snapshot alive after an announcement has
    // emptied the cache, so a walk in flight finishes on the bytes it started on.
    using EntryPtr = std::shared_ptr<const Entry>;

    // Where one #include operand resolved.
    struct Resolution
    {
        // The spelling the search produced, unnormalized: the includer hands it
        // to shaderc as source_name, so nested relative directives inside the
        // file resolve against it and not against the normalized form.
        std::filesystem::path Candidate;
        // Candidate's directory: the first place the file's own directives search.
        std::filesystem::path CandidateDir;
        // NormalizedKey(Candidate): the identity the file is held, deduped and
        // reported by.
        std::string Key;
    };
    using ResolutionPtr = std::shared_ptr<const Resolution>;

    static ShaderSourceCache& Get();

    // Absolute and lexically normal: the key a file is held by.
    static std::string NormalizedKey(const std::filesystem::path& path);

    // The held entry for `normalizedKey`, reading `path` on the first request.
    // Null when the file cannot be read; a failed read is not remembered.
    EntryPtr Read(const std::filesystem::path& path, const std::string& normalizedKey);

    // The first `requestingDir / operand` (skipped when the directory is empty),
    // then `root / operand` for each of `roots` in order, that exists: the
    // compiler includer's search order.
    // Null when none does. `roots` must be the list `rootsSignature` was built
    // from (MakeRootsSignature).
    ResolutionPtr Resolve(const std::filesystem::path& requestingDir, const std::vector<std::filesystem::path>& roots,
                          const std::string& rootsSignature, const std::string& operand);

    // The memo key of a search-root list: built once per walk, not per directive.
    static std::string MakeRootsSignature(const std::vector<std::filesystem::path>& roots);

    // Forget every held file and every resolution. A read or a resolution that
    // started before this call is handed to its caller but never stored.
    void InvalidateAll();

    // Advanced by every InvalidateAll. Equal before and after a span of work
    // means no announcement landed inside it.
    uint64_t Generation();

private:
    std::mutex m_Mutex;
    // A read stores its result only when the generation it started under is
    // still current.
    uint64_t m_Generation = 0;
    // Read and Resolve hold no iterator past the lock and return a copy of the
    // pointer, so the flat maps' unstable iterators are safe.
    FastHashMap<std::string, EntryPtr> m_Entries;
    // rootsSignature -> (requestingDir '\n' operand) -> resolution, null for
    // "resolves nowhere". The prewarm looks one up per directive.
    FastHashMap<std::string, FastHashMap<std::string, ResolutionPtr>> m_Resolutions;
};

}} // namespace GameEngine::Rendering
