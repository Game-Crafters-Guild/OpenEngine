// A tooltip or a range for an engine component the scanner reflects is a marker on the header
// field. A hand-written GE_REFLECT_FIELD_TOOLTIP, GE_REFLECT_FIELD_RANGE, SetReflectedFieldTooltip
// or SetReflectedFieldRange in an engine or package source for such a type is a second source for
// the same field: it fails here, naming the file and the type.
#include "ComponentScanner.h"

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace fs = std::filesystem;

namespace
{

std::string ReadFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// The simple names of the structs an engine-mode scan of the engine's component roots reflects,
// read from the report's "--- REFLECTED ---" list ("  Name (fieldCount)").
std::set<std::string> ScannedComponentNames()
{
    const fs::path engine = fs::path(GE_ENGINE_INCLUDE_DIR).parent_path();
    std::vector<std::string> args{"--in", (engine / "Include" / "Components").generic_string(), "--dry-run"};
    for (const fs::directory_entry& module : fs::directory_iterator(engine / "Modules"))
        if (const fs::path root = module.path() / "Include" / "Components"; fs::is_directory(root))
            args.insert(args.end(), {"--in", root.generic_string()});
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(0, GameEngine::ComponentScanner::Run(args, out, err)) << err.str();

    std::set<std::string> names;
    std::istringstream report(out.str());
    std::string line;
    bool inReflected = false;
    const std::regex entry(R"(^  (\w+) \()");
    while (std::getline(report, line))
    {
        if (line.starts_with("---"))
            inReflected = line == "--- REFLECTED ---";
        else if (std::smatch match; inReflected && std::regex_search(line, match, entry))
            names.insert(match[1]);
    }
    return names;
}

} // namespace

TEST(HandMetadataDrift, NoHandWrittenTooltipOrRangeNamesAScannedComponent)
{
    const std::set<std::string> scanned = ScannedComponentNames();
    ASSERT_GT(scanned.size(), 100u);

    const std::regex binding(
        R"((?:GE_REFLECT_FIELD_(?:TOOLTIP|RANGE)\(\s*|SetReflectedField(?:Tooltip|Range)<\s*)(?:[\w]+::)*(\w+))");
    const fs::path repo = fs::path(GE_ENGINE_INCLUDE_DIR).parent_path().parent_path();
    std::vector<std::string> drift;
    for (const char* tree : {"Engine", "Packages"})
    {
        if (!fs::is_directory(repo / tree))
            continue;
        for (const fs::directory_entry& file : fs::recursive_directory_iterator(repo / tree))
        {
            if (!file.is_regular_file() || file.path().extension() != ".cpp")
                continue;
            const std::string text = ReadFile(file.path());
            for (auto it = std::sregex_iterator(text.begin(), text.end(), binding); it != std::sregex_iterator(); ++it)
                if (scanned.count((*it)[1]) != 0)
                    drift.push_back(fs::relative(file.path(), repo).generic_string() + " binds " + (*it)[1].str());
        }
    }
    std::string message = "Write these as // @ge-tooltip or // @ge-range markers on the header fields:\n";
    for (const std::string& line : drift)
        message += "  " + line + "\n";
    EXPECT_TRUE(drift.empty()) << message;
}
