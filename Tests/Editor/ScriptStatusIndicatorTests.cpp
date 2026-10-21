// The bottom bar's scripts status states the C# side and the C++ side together (issue #4050):
// before it, "Scripts loaded" reported the native modules only, so a project whose C# assembly never
// loaded read as healthy.

#include "Scripting/ScriptStatusIndicator.h"

#include "Scripting/ScriptManager.h"

#include "UI/Controls/Label.h"

#include <gtest/gtest.h>

namespace
{
using GameEngine::ScriptAssemblyState;
using GameEngine::ScriptAssemblyStatus;
using GameEngine::Editor::NativeScriptState;
using GameEngine::Editor::ScriptStatusDisplay;
using GameEngine::Editor::ScriptStatusIndicator;

ScriptStatusDisplay Describe(NativeScriptState native, ScriptAssemblyState managed, std::uint32_t types = 0)
{
    return ScriptStatusIndicator::Describe(native, ScriptAssemblyStatus{managed, types});
}

TEST(ScriptStatusIndicator, EachManagedStateReadsAsItsOwnStatus)
{
    EXPECT_EQ(Describe(NativeScriptState::None, ScriptAssemblyState::NotLoaded),
              (ScriptStatusDisplay{"C# scripts not loaded", ""}));
    EXPECT_EQ(Describe(NativeScriptState::None, ScriptAssemblyState::Building),
              (ScriptStatusDisplay{"Building C# scripts...", "is-building"}));
    EXPECT_EQ(Describe(NativeScriptState::None, ScriptAssemblyState::Loaded, 1),
              (ScriptStatusDisplay{"C# scripts loaded (1 type)", "is-ok"}));
    EXPECT_EQ(Describe(NativeScriptState::None, ScriptAssemblyState::Loaded, 3),
              (ScriptStatusDisplay{"C# scripts loaded (3 types)", "is-ok"}));
    EXPECT_EQ(Describe(NativeScriptState::None, ScriptAssemblyState::Failed),
              (ScriptStatusDisplay{"C# build failed - see Log", "is-fail"}));
    EXPECT_EQ(Describe(NativeScriptState::Loaded, ScriptAssemblyState::Disabled),
              (ScriptStatusDisplay{"C# scripting is off in this build, C++ modules loaded", "is-ok"}));
}

// The scripts side reads Disabled in a build without C# scripting, which is what lets the bar
// say so; a scripting build starts from NotLoaded.
TEST(ScriptStatusIndicator, ScriptManagerReportsABuildWithoutCSharpScriptingAsDisabled)
{
    GameEngine::ScriptManager scripts;
#if GE_ENABLE_SCRIPTING
    EXPECT_EQ(scripts.GetScriptAssemblyStatus().State, ScriptAssemblyState::NotLoaded);
#else
    EXPECT_EQ(scripts.GetScriptAssemblyStatus().State, ScriptAssemblyState::Disabled);
#endif
}

TEST(ScriptStatusIndicator, BothSidesShareOneStatusAndTheWorseSideSetsItsColor)
{
    EXPECT_EQ(Describe(NativeScriptState::Loaded, ScriptAssemblyState::NotLoaded),
              (ScriptStatusDisplay{"C# scripts not loaded, C++ modules loaded", "is-ok"}));
    EXPECT_EQ(Describe(NativeScriptState::Building, ScriptAssemblyState::Loaded, 2),
              (ScriptStatusDisplay{"C# scripts loaded (2 types), building C++ modules...", "is-building"}));
    EXPECT_EQ(Describe(NativeScriptState::Failed, ScriptAssemblyState::Building),
              (ScriptStatusDisplay{"Building C# scripts..., C++ build failed - using last good build", "is-fail"}));
}

TEST(ScriptStatusIndicator, AFailureOnEitherSideBringsTheLogForwardOnce)
{
    GameEngine::Label label;
    int logShown = 0;
    ScriptStatusIndicator indicator([&label]() { return &label; }, [&logShown]() { ++logShown; });

    indicator.Update(ScriptAssemblyStatus{ScriptAssemblyState::Building, 0});
    EXPECT_EQ(label.GetText(), "Building C# scripts...");
    indicator.Update(ScriptAssemblyStatus{ScriptAssemblyState::Failed, 0});
    indicator.Update(ScriptAssemblyStatus{ScriptAssemblyState::Failed, 0});
    EXPECT_EQ(label.GetText(), "C# build failed - see Log");
    EXPECT_EQ(logShown, 1) << "a failure that persists across frames must not keep pulling the Log forward";

    indicator.Update(ScriptAssemblyStatus{ScriptAssemblyState::Loaded, 4});
    indicator.SetNativeState(NativeScriptState::Failed);
    EXPECT_EQ(label.GetText(), "C# scripts loaded (4 types), C++ build failed - using last good build");
    EXPECT_EQ(logShown, 2);
}
} // namespace
