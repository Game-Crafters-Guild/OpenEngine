// The Scene View's layer filter: every layer for a project that never set one, and a saved filter that
// comes back for the same project and stays out of another project's views.

#include "SceneView/SceneViewLayerFilter.h"

#include "../TestTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>

using namespace GameEngine;
namespace gr = GameEngine::Rendering;

TEST(SceneViewLayerFilterTest, APersistedFilterComesBackForItsProjectOnly)
{
    const TestUtils::ScopedTempDir first(TestUtils::MakeUniqueTempDirectory("ge_scene_view_layers_a"));
    const TestUtils::ScopedTempDir second(TestUtils::MakeUniqueTempDirectory("ge_scene_view_layers_b"));
    std::filesystem::create_directories(first.Path() / ".Editor");
    std::filesystem::create_directories(second.Path() / ".Editor");

    auto& filter = Editor::SceneViewLayerFilter::Get();
    EXPECT_EQ(filter.Layers(first.Path()), gr::RenderLayerMask::All())
        << "a project that never set the filter draws every layer";

    const gr::RenderLayerMask withoutFoliage{gr::RenderLayerMask::All().Bits & ~gr::RenderLayerMask::FromLayer(3).Bits};
    filter.SetLayers(first.Path(), withoutFoliage);
    EXPECT_EQ(filter.Layers(second.Path()), gr::RenderLayerMask::All()) << "another project keeps its own";
    EXPECT_EQ(filter.Layers(first.Path()), withoutFoliage)
        << "reopening the project reads the saved filter back from its user settings";
}
