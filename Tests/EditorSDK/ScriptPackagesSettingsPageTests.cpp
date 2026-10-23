// The Script Packages page's registration shape, and its list against a temporary scripts csproj:
// the rows are the file's packages, an add writes one PackageReference and refuses a duplicate, a
// removal rewrites the file, a removal the file refuses keeps its row, and the version field writes
// a valid version and refuses an invalid one. Under central package versions the versions go into
// Directory.Packages.props.

#include "Editor/Settings/ScriptPackagesList.h"
#include "Editor/Settings/ScriptPackagesSettingsPage.h"

#include "Editor/Settings/EditorSettingsRegistry.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextField.h"

#include "../TestTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <variant>
#include <vector>

namespace ed = GameEngine::Editor;
namespace fs = std::filesystem;
using GameEngine::Label;
using GameEngine::TextField;
using GameEngine::UIElement;

namespace
{

const std::string kProject = "<Project Sdk=\"Microsoft.NET.Sdk\">\n"
                             "  <Import Project=\"GameEngine.Scripts.Engine.props\" />\n"
                             "  <ItemGroup>\n"
                             "    <PackageReference Include=\"Humanizer.Core\" Version=\"2.14.1\" />\n"
                             "    <PackageReference Include=\"Newtonsoft.Json\" Version=\"13.0.3\" />\n"
                             "  </ItemGroup>\n"
                             "  <Import Project=\"GameEngine.Scripts.Engine.targets\" />\n"
                             "</Project>\n";

// Every element under `root` that carries `className`, in tree order.
void CollectByClass(UIElement* root, const std::string& className, std::vector<UIElement*>& found)
{
    for (const auto& child : root->GetChildren())
    {
        if (child->HasClass(className))
            found.push_back(child.get());
        CollectByClass(child.get(), className, found);
    }
}

std::vector<UIElement*> ByClass(UIElement& root, const std::string& className)
{
    std::vector<UIElement*> found;
    CollectByClass(&root, className, found);
    return found;
}

class ScriptPackagesSettingsPageTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Root = GameEngine::TestUtils::MakeUniqueTempDirectory("ge_script_packages_page");
        fs::create_directories(m_Root);
        m_Project = m_Root / "GameEngine.Scripts.csproj";
        std::ofstream(m_Project, std::ios::binary) << kProject;
    }
    void TearDown() override
    {
        std::error_code ec;
        fs::permissions(m_Project, fs::perms::owner_write, fs::perm_options::add, ec);
        fs::remove_all(m_Root, ec);
    }

    std::string ProjectText() const
    {
        std::ifstream in(m_Project, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    // The package ids the list shows, in row order.
    static std::vector<std::string> RowIds(ed::ScriptPackagesList& list)
    {
        std::vector<std::string> ids;
        for (UIElement* row : ByClass(list, "script-packages-rows"))
        {
            for (UIElement* id : ByClass(*row, "script-packages-id"))
            {
                if (const auto* label = dynamic_cast<Label*>(id))
                    ids.push_back(label->GetText());
            }
        }
        return ids;
    }

    static std::string Status(ed::ScriptPackagesList& list)
    {
        const std::vector<UIElement*> status = ByClass(list, "script-packages-status");
        const auto* label = status.empty() ? nullptr : dynamic_cast<Label*>(status.front());
        return label ? label->GetText() : std::string();
    }

    static void TypeNewPackage(ed::ScriptPackagesList& list, const std::string& id, const std::string& version)
    {
        dynamic_cast<TextField&>(*ByClass(list, "script-packages-new-id").at(0)).SetValue(id);
        dynamic_cast<TextField&>(*ByClass(list, "script-packages-new-version").at(0)).SetValue(version);
    }

    fs::path m_Root;
    fs::path m_Project;
};

TEST_F(ScriptPackagesSettingsPageTest, RegistersUnderProjectSettingsAndListsTheFilesPackages)
{
    ed::RegisterScriptPackagesSettingsCategory();
    ed::SettingsCategoryDescriptor page;
    ASSERT_TRUE(ed::EditorSettingsRegistry::Get().TryGet("scriptPackages", page));
    EXPECT_EQ(page.Title, "Script Packages");
    EXPECT_EQ(page.Group, ed::SettingsCategoryGroup::ProjectSettings);
    ASSERT_EQ(page.Fields.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<ed::SettingsFieldDescriptor::CustomField>(page.Fields[0].Control));

    ed::ScriptPackagesList list(m_Project);
    EXPECT_EQ(RowIds(list), (std::vector<std::string>{"Humanizer.Core", "Newtonsoft.Json"}));
    const std::vector<UIElement*> removes = ByClass(list, "script-packages-remove");
    ASSERT_EQ(removes.size(), 2u);
    EXPECT_EQ(removes[0]->GetTooltip(), "Remove this package from the project file");
}

TEST_F(ScriptPackagesSettingsPageTest, AddWritesOnePackageReferenceAndRefusesADuplicate)
{
    ed::ScriptPackagesList list(m_Project);
    TypeNewPackage(list, "Serilog", "4.0.0");
    list.AddPackage();
    EXPECT_NE(ProjectText().find("<PackageReference Include=\"Serilog\" Version=\"4.0.0\" />"), std::string::npos)
        << ProjectText();
    EXPECT_EQ(RowIds(list), (std::vector<std::string>{"Humanizer.Core", "Newtonsoft.Json", "Serilog"}));

    const std::string before = ProjectText();
    TypeNewPackage(list, "newtonsoft.json", "12.0.1");
    list.AddPackage();
    EXPECT_EQ(ProjectText(), before) << "a duplicate id, compared without case, is refused";
    EXPECT_EQ(Status(list), "Newtonsoft.Json is already in the list: change its version there.");
}

TEST_F(ScriptPackagesSettingsPageTest, RemovalRewritesTheFile)
{
    ed::ScriptPackagesList list(m_Project);
    list.RemovePackage("Humanizer.Core");
    EXPECT_EQ(ProjectText().find("Humanizer.Core"), std::string::npos) << ProjectText();
    EXPECT_NE(ProjectText().find("Newtonsoft.Json"), std::string::npos) << "the other package stays";
    EXPECT_EQ(RowIds(list), (std::vector<std::string>{"Newtonsoft.Json"}));
}

TEST_F(ScriptPackagesSettingsPageTest, RemovalTheFileRefusesKeepsTheRow)
{
    ed::ScriptPackagesList list(m_Project);
    fs::permissions(m_Project, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
                    fs::perm_options::remove);
    list.RemovePackage("Humanizer.Core");
    EXPECT_NE(ProjectText().find("Humanizer.Core"), std::string::npos);
    EXPECT_EQ(RowIds(list), (std::vector<std::string>{"Humanizer.Core", "Newtonsoft.Json"}))
        << "the page and the file disagree";
    EXPECT_NE(Status(list).find("Cannot write"), std::string::npos) << Status(list);
}

TEST_F(ScriptPackagesSettingsPageTest, VersionFieldWritesAValidVersionAndRefusesAnInvalidOne)
{
    ed::ScriptPackagesList list(m_Project);
    UIElement& rows = *ByClass(list, "script-packages-rows").at(0);
    const std::vector<UIElement*> versions = ByClass(rows, "script-packages-version");
    ASSERT_EQ(versions.size(), 2u);
    auto& humanizer = dynamic_cast<TextField&>(*versions[0]);

    humanizer.SetValue("2.14.2");
    humanizer.NotifyValueChanged();
    EXPECT_NE(ProjectText().find("<PackageReference Include=\"Humanizer.Core\" Version=\"2.14.2\" />"),
              std::string::npos)
        << ProjectText();

    const std::string before = ProjectText();
    humanizer.SetValue("~2.14");
    humanizer.NotifyValueChanged();
    EXPECT_EQ(ProjectText(), before) << "an invalid version is refused";
    EXPECT_EQ(humanizer.GetValue(), "2.14.2") << "the field shows the version the file holds";
    EXPECT_NE(Status(list).find("'~2.14' is not a NuGet version"), std::string::npos) << Status(list);
}

TEST_F(ScriptPackagesSettingsPageTest, CentralVersionsGoIntoDirectoryPackagesProps)
{
    std::ofstream(m_Project, std::ios::binary | std::ios::trunc)
        << "<Project Sdk=\"Microsoft.NET.Sdk\">\n"
           "  <ItemGroup>\n"
           "    <PackageReference Include=\"Humanizer.Core\" />\n"
           "  </ItemGroup>\n"
           "</Project>\n";
    const fs::path props = m_Root / "Directory.Packages.props";
    std::ofstream(props, std::ios::binary)
        << "<Project>\n"
           "  <PropertyGroup><ManagePackageVersionsCentrally>true</ManagePackageVersionsCentrally></PropertyGroup>\n"
           "  <ItemGroup>\n"
           "    <PackageVersion Include=\"Humanizer.Core\" Version=\"2.14.1\" />\n"
           "  </ItemGroup>\n"
           "</Project>\n";
    const auto propsText = [&props]
    {
        std::ifstream in(props, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };

    ed::ScriptPackagesList list(m_Project);
    UIElement& rows = *ByClass(list, "script-packages-rows").at(0);
    auto& humanizer = dynamic_cast<TextField&>(*ByClass(rows, "script-packages-version").at(0));
    EXPECT_EQ(humanizer.GetValue(), "2.14.1") << "the row shows the central version";

    TypeNewPackage(list, "Serilog", "4.0.0");
    list.AddPackage();
    EXPECT_NE(ProjectText().find("<PackageReference Include=\"Serilog\" />"), std::string::npos) << ProjectText();
    EXPECT_EQ(ProjectText().find("Version="), std::string::npos) << "NuGet refuses a Version here (NU1008)";
    EXPECT_NE(propsText().find("<PackageVersion Include=\"Serilog\" Version=\"4.0.0\" />"), std::string::npos)
        << propsText();

    humanizer.SetValue("2.14.2");
    humanizer.NotifyValueChanged();
    EXPECT_NE(propsText().find("<PackageVersion Include=\"Humanizer.Core\" Version=\"2.14.2\" />"), std::string::npos)
        << propsText();
    EXPECT_EQ(ProjectText().find("Version="), std::string::npos) << ProjectText();

    list.RemovePackage("Serilog");
    EXPECT_EQ(ProjectText().find("Serilog"), std::string::npos) << ProjectText();
    EXPECT_NE(propsText().find("<PackageVersion Include=\"Serilog\" Version=\"4.0.0\" />"), std::string::npos)
        << "other projects may share the file";
    EXPECT_NE(Status(list).find("Its version stays in " + props.string()), std::string::npos) << Status(list);

    fs::permissions(props, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
                    fs::perm_options::remove);
    TypeNewPackage(list, "Newtonsoft.Json", "13.0.3");
    list.AddPackage();
    fs::permissions(props, fs::perms::owner_write, fs::perm_options::add);
    EXPECT_EQ(ProjectText().find("Newtonsoft.Json"), std::string::npos) << "a refused version adds no reference";
    EXPECT_NE(Status(list).find("Cannot write " + props.string() + ". Clear its read-only flag"), std::string::npos)
        << Status(list);
}

} // namespace
