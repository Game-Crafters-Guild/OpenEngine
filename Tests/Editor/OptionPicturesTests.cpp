// A picture of what an option does (a preset in a dropdown, a tile of a picture choice field) is a CSS
// class the declaring code names beside the option; theme/option-pictures.css says which image that
// class is. A declared class with no rule shows nothing and a rule naming a missing file only warns at
// load, so the three are checked against each other here, from the sources, as
// DropdownOptionIconsTests does for the icons.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

namespace
{

constexpr const char* kPictureStylesheet = "Assets/UI/theme/option-pictures.css";

std::filesystem::path EditorPath(const std::string& relativePath)
{
    return std::filesystem::path(GE_EDITOR_SOURCE_DIR) / relativePath;
}

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::set<std::string> PictureClassesIn(const std::string& text)
{
    static const std::regex pattern(R"(option-picture--[a-z0-9-]+)");
    std::set<std::string> found;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), pattern); it != std::sregex_iterator(); ++it)
        found.insert(it->str());
    return found;
}

// Every class the editor's sources declare, found rather than listed, so the next preset list is
// checked without editing this file.
std::set<std::string> DeclaredPictureClasses()
{
    std::set<std::string> declared;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(EditorPath("Source")))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".cpp")
            declared.merge(PictureClassesIn(ReadFile(entry.path())));
    }
    return declared;
}

} // namespace

TEST(OptionPicturesTests, EveryDeclaredClassHasARuleAndEveryRuleIsDeclared)
{
    const std::string sheet = ReadFile(EditorPath(kPictureStylesheet));
    ASSERT_FALSE(sheet.empty()) << kPictureStylesheet << " did not read";
    const std::set<std::string> declared = DeclaredPictureClasses();
    EXPECT_FALSE(declared.empty()) << "no editor source declares an option picture";
    for (const std::string& className : declared)
        EXPECT_NE(sheet.find("." + className + " {"), std::string::npos)
            << className << " is declared but has no rule in " << kPictureStylesheet;
    for (const std::string& className : PictureClassesIn(sheet))
        EXPECT_EQ(declared.count(className), 1u) << className << " has a rule but nothing declares it";
}

TEST(OptionPicturesTests, EveryRuleNamesTheExistingFileItsClassIsNamedAfter)
{
    // One stem per picture: `.option-picture--<stem>` means `Icons/OptionPictures/<stem>.png`.
    const std::string sheet = ReadFile(EditorPath(kPictureStylesheet));
    ASSERT_FALSE(sheet.empty()) << kPictureStylesheet << " did not read";
    static const std::regex rule(
        R"RX(\.option-picture--([a-z0-9-]+)\s*\{[^}]*url\("editor:(Icons/OptionPictures/([^"]+)\.png)"\))RX");
    size_t checked = 0;
    for (auto it = std::sregex_iterator(sheet.begin(), sheet.end(), rule); it != std::sregex_iterator(); ++it)
    {
        EXPECT_EQ((*it)[1].str(), (*it)[3].str()) << "class stem and file stem differ";
        EXPECT_TRUE(std::filesystem::exists(EditorPath("Assets/" + (*it)[2].str())))
            << (*it)[2].str() << " is named by " << kPictureStylesheet << " but does not exist";
        ++checked;
    }
    EXPECT_EQ(checked, PictureClassesIn(sheet).size()) << "a rule does not match the one-stem-per-picture shape";
}
