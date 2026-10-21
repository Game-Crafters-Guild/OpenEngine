#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine::Rendering
{
/// The file a material variant cook writes its program index to, in its cache root.
inline constexpr const char* kShaderProgramIndexFileName = "program-index.bin";

/// Bumped whenever the file layout or the input key a material build derives
/// (BuildMaterialToShaderPackage) changes; an index written under another version is ignored.
inline constexpr uint32_t kShaderProgramIndexVersion = 2;

struct MaterialBuildContext;

/// A cooked shader cache's index from a material variant's build inputs to the program
/// that serves them. The material variant cook writes it beside the programs it cooked;
/// a runtime that ships that cache looks a variant up by its inputs and loads the program,
/// without composing the variant's source and hashing its include closure to find the key.
/// An input the index does not hold falls through to that path.
///
/// An index is valid only for the compile inputs it was written under (CurrentCompileInputs:
/// the cache key's format and SPIR-V target and the shader profile) and for the shader sources its
/// programs were composed from: it records each source file (ShaderSourceIdentity) with a hash of
/// its content, and Load refuses it when one is missing or changed under the runtime's roots, when
/// it was written for other inputs, or under another kShaderProgramIndexVersion. A stale index is
/// never trusted: a variant it would have served composes from the shipped sources instead.
class ShaderProgramIndex
{
public:
    struct Entry
    {
        /// The build's input key (MaterialBuildResult::programIndexKey).
        uint64_t InputKey = 0;
        /// The program's package, relative to the cache root that holds the index.
        std::string PackagePath;
    };

    struct Counts
    {
        uint64_t Hits = 0;
        uint64_t Misses = 0;
    };

    /// The compile inputs this process keys programs under: what an index must have been
    /// written for to be used here.
    static std::string CurrentCompileInputs();

    /// Writes `entries` as the index at `path`, for the current compile inputs and for `sources`,
    /// the shader files the entries' programs were composed from, named and hashed under
    /// `context`'s roots (files under none of them, such as surfaces generated under the cache
    /// root, are not recorded). Entries are sorted by input key; an input key listed twice must
    /// name one package. Returns false with `outError` set when the file cannot be written, an
    /// entry is invalid or a source cannot be read.
    static bool Write(const std::filesystem::path& path, std::vector<Entry> entries,
                      const std::vector<std::filesystem::path>& sources, const MaterialBuildContext& context,
                      std::string* outError);

    /// Loads the index at `path`, checking its recorded sources against `context`'s roots. Null
    /// when there is no file, when it cannot be read or parsed, when it was written under another
    /// version or for other compile inputs, or when a recorded source is missing or changed;
    /// `outReason` then says which.
    static std::shared_ptr<const ShaderProgramIndex> Load(const std::filesystem::path& path,
                                                          const MaterialBuildContext& context,
                                                          std::string* outReason);

    /// The package that serves `inputKey`, as an absolute path under the index's cache root,
    /// or empty when the index does not hold the key.
    std::filesystem::path Find(uint64_t inputKey) const;

    /// Records how a lookup ended: served from the package the index names (a hit), or composed
    /// and hashed because the index does not hold the key or its package could not be served.
    void CountLookup(bool served) const;

    /// Lookups since the load, by outcome (CountLookup).
    Counts GetCounts() const;

    size_t Size() const { return m_Entries.size(); }
    const std::filesystem::path& CacheRoot() const { return m_CacheRoot; }

private:
    std::vector<Entry> m_Entries; // sorted by InputKey, unique
    std::filesystem::path m_CacheRoot;
    mutable std::atomic<uint64_t> m_Hits{0};
    mutable std::atomic<uint64_t> m_Misses{0};
};
} // namespace GameEngine::Rendering
