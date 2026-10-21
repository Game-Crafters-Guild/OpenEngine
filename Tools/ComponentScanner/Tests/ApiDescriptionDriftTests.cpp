// The committed script API description, Engine/Scripting/engine-api/<Module>.json, and the
// committed C# binding, Managed/Scripting.Bindings/Generated/<Module>.g.cs, are what a fresh scan
// of the engine's headers writes, file for file and byte for byte, and the native binding the
// build generates carries the committed description's hash. A marked declaration changed without
// its files regenerated fails here, naming the file and the command that regenerates it.
#include "ComponentScanner.h"

#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string_view>

namespace fs = std::filesystem;

namespace
{

constexpr const char* kRegenerate =
    "Regenerate it with `cmake --build --preset <preset> --target UpdateEngineApi` and commit Engine/Scripting/engine-api "
    "and Managed/Scripting.Bindings/Generated with the header change.";

std::string ReadFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::string Utf8Text(const fs::path& path)
{
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

// Every file of a directory whose name ends in `suffix`, by file name.
std::map<std::string, std::string> FilesEndingIn(const fs::path& dir, std::string_view suffix)
{
    std::map<std::string, std::string> files;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (const std::string name = Utf8Text(it->path().filename()); name.ends_with(suffix))
            files[name] = ReadFile(it->path());
    return files;
}

// Compares each committed file with the fresh one of the same name, both ways.
void ExpectSameFiles(const std::map<std::string, std::string>& committed, const std::map<std::string, std::string>& fresh,
                     std::string_view committedDir, std::string_view suffix)
{
    for (const auto& [name, text] : fresh)
    {
        const auto it = committed.find(name);
        EXPECT_TRUE(it != committed.end()) << committedDir << "/" << name << " is missing. " << kRegenerate;
        if (it != committed.end())
            EXPECT_TRUE(it->second == text)
                << committedDir << "/" << name << " is out of date with its module's script API headers. " << kRegenerate;
    }
    for (const auto& [name, text] : committed)
    {
        const std::string module = name.substr(0, name.size() - suffix.size());
        EXPECT_TRUE(fresh.count(name) != 0) << committedDir << "/" << name << " has no " << module << "ScriptApi.h. "
                                            << kRegenerate;
    }
}

// The value of `"hash": "..."` in a description, or of `GE_<Module>_Hash = ...ull;` in a native binding header.
std::string HashIn(const std::string& text, std::string_view key, std::string_view end)
{
    const std::size_t start = text.find(key);
    if (start == std::string::npos)
        return {};
    const std::size_t value = start + key.size();
    return text.substr(value, text.find(end, value) - value);
}

} // namespace

TEST(ApiDescriptionDrift, CommittedDescriptionMatchesTheEngineHeaders)
{
    GameEngine::TestUtils::ScopedTempDir scratch(GameEngine::TestUtils::MakeUniqueTempDirectory("ComponentScannerDrift"));
    const fs::path fresh = scratch.Path() / "engine-api";
    const fs::path native = scratch.Path() / "native";
    const fs::path csharp = scratch.Path() / "csharp";
    std::vector<std::string> args{"--api-modules", GE_ENGINE_MODULES_DIR, "--emit-api", Utf8Text(fresh),
                                  "--emit-native", Utf8Text(native),       "--emit-csharp", Utf8Text(csharp)};
    for (const char* search : {GE_ENGINE_INCLUDE_DIR, GE_ENGINE_ECS_MODULES_DIR})
        args.insert(args.end(), {"--api-search", search});
    std::ostringstream out;
    std::ostringstream err;

    const int exitCode = GameEngine::ComponentScanner::Run(args, out, err);

    ASSERT_EQ(0, exitCode) << err.str();
    const std::map<std::string, std::string> committed = FilesEndingIn(fs::path(GE_ENGINE_API_DESCRIPTION_DIR), ".json");
    ExpectSameFiles(committed, FilesEndingIn(fresh, ".json"), "Engine/Scripting/engine-api", ".json");
    ExpectSameFiles(FilesEndingIn(fs::path(GE_CSHARP_BINDINGS_DIR), ".g.cs"), FilesEndingIn(csharp, ".g.cs"),
                    "Managed/Scripting.Bindings/Generated", ".g.cs");
    for (const auto& [name, header] : FilesEndingIn(native, ".script.gen.h"))
    {
        const std::string module = name.substr(0, name.size() - std::string_view(".script.gen.h").size());
        const auto description = committed.find(module + ".json");
        ASSERT_TRUE(description != committed.end()) << name;
        EXPECT_EQ(HashIn(description->second, "\"hash\": \"", "\""), HashIn(header, "_Hash = ", "ull;"))
            << "the native binding of " << module << " carries another hash than Engine/Scripting/engine-api/" << module
            << ".json. " << kRegenerate;
    }
}
