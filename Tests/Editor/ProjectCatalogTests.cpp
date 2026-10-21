// A catalog source the editor refuses names the rule it broke, so the picker can tell the user
// what to fix: a git or zip URL that is not http(s), or a folder outside the repository.

#include <gtest/gtest.h>

#include "Projects/ProjectCatalog.h"

#include <string>

using GameEngine::Editor::ParseProjectCatalog;
using GameEngine::Editor::ProjectCatalog;
using GameEngine::Editor::ProjectSourceType;

TEST(ProjectCatalog, ARefusedSourceNamesTheRuleItBroke)
{
    const std::string manifest = R"({
      "schemaVersion": 1,
      "projects": [
        { "id": "local-url", "name": "Local URL",
          "source": { "type": "git", "url": "C:/Dev/catalog", "path": "projects/breakout" } },
        { "id": "escaping-path", "name": "Escaping path",
          "source": { "type": "git", "url": "https://example.com/repo", "path": "../outside" } },
        { "id": "fetchable", "name": "Fetchable",
          "source": { "type": "git", "url": "https://example.com/repo", "path": "projects/pong" } }
      ]
    })";
    ProjectCatalog catalog;
    std::string error;
    ASSERT_TRUE(ParseProjectCatalog(manifest, {}, catalog, &error)) << error;
    ASSERT_EQ(catalog.Entries.size(), 3u);

    EXPECT_EQ(catalog.Entries[0].Source.Type, ProjectSourceType::Unsupported);
    EXPECT_EQ(catalog.Entries[0].Source.RefusalReason,
              "Community projects are fetched over http(s) only; this project's source URL is 'C:/Dev/catalog'.");

    EXPECT_EQ(catalog.Entries[1].Source.Type, ProjectSourceType::Unsupported);
    EXPECT_EQ(catalog.Entries[1].Source.RefusalReason,
              "Community projects are fetched from a folder inside their repository; this project's source "
              "path is '../outside'.");

    EXPECT_EQ(catalog.Entries[2].Source.Type, ProjectSourceType::Git);
    EXPECT_TRUE(catalog.Entries[2].Source.RefusalReason.empty());
}
