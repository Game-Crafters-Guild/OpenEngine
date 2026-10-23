// The captured pointer's lock.
//
// A value drag asks UIManager to lock the pointer once the press has become a
// drag; the host then hides the cursor and lets it travel past the screen edge.
// These tests drive a FloatField's drag-to-change through a real UIManager and
// pin what reaches the pointer lock callback, that the drag keeps reading
// moves once the pointer runs past the window's left edge, and that Escape or a
// second button cancels the drag and puts back the start value. Hiding the platform
// cursor and showing it again where the drag began is the window's job, checked
// in the running editor.

#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "Rendering/Core/Device.h"
#include "UI/Controls/FloatField.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIManager.h"
#include "UIRgTestHarness.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

struct PointerLockFixture
{
    // First member: destroyed last, after this fixture's UIManager released its buffers.
    SharedDeviceReleaseRetirement ReleaseRetirement;
    IDevice* Dev = nullptr;
    std::unique_ptr<UIManager> Ui;
    std::unique_ptr<UiRgHarness> Rg;
    FloatField* Field = nullptr;
    std::vector<bool> LockEvents;

    bool Init()
    {
        Dev = SharedHeadlessDevice();
        if (!Dev)
            return false;

        UIRegistration::RegisterBuiltInControls();

        auto root = std::make_unique<UIElement>();
        root->SetId("root");
        auto field = std::make_unique<FloatField>();
        Field = field.get();
        field->SetId("field");
        field->EnableDragToChange();
        root->AddChild(std::move(field));

        Ui = std::make_unique<UIManager>(Dev);
        Ui->SetRoot(std::move(root));
        Ui->SetPointerLockCallback([this](bool locked) { LockEvents.push_back(locked); });

        const auto css = std::filesystem::temp_directory_path() /
                         ("ui_pointer_lock_" + std::to_string(reinterpret_cast<uintptr_t>(this)) + ".css");
        {
            std::ofstream f(css);
            f << "#root { display: flex; flex-direction: column; width: 400px; height: 120px; }\n"
                 "#field { width: 200px; height: 26px; font-size: 14px; }\n"
                 ".field-editor { width: 100%; height: 100%; padding: 0 4px; }\n";
        }
        if (!Ui->AttachStyleFromFile(css.string()))
            return false;

        Field->SetValue(1.0f);
        Rg = std::make_unique<UiRgHarness>(Dev);
        for (int i = 0; i < 4; ++i)
            Pump();
        return Field->GetLayoutWidth() > 0.0f;
    }

    void Pump()
    {
        Ui->Update(0.016f, /*interactive=*/true);
        DriveUiRender(*Ui, *Rg);
    }

    float CentreX() const { return Field->GetLayoutX() + Field->GetLayoutWidth() * 0.5f; }
    float CentreY() const { return Field->GetLayoutY() + Field->GetLayoutHeight() * 0.5f; }

    void PressAtCentre()
    {
        Ui->OnMouseMove(CentreX(), CentreY());
        Pump();
        Ui->OnMouseButton(0, true);
        Pump();
    }

    void MoveTo(float x)
    {
        Ui->OnMouseMove(x, CentreY());
        Pump();
    }

    void Release()
    {
        Ui->OnMouseButton(0, false);
        Pump();
    }

    ~PointerLockFixture()
    {
        Rg.reset();
        Ui.reset();
    }
};

#define REQUIRE_POINTER_LOCK_FIXTURE(fx, inited)                                      \
    do                                                                                \
    {                                                                                 \
        if (!(fx).Dev)                                                                \
            GTEST_SKIP() << "Device init failed";                                     \
        ASSERT_TRUE(inited);                                                          \
    } while (false)

} // namespace

// A click, even one that jitters by a pixel or two, never hides the cursor:
// the lock waits for the press to become a drag.
TEST(PointerLockTests, AClickDoesNotLockThePointer)
{
    PointerLockFixture fx;
    const bool inited = fx.Init();
    REQUIRE_POINTER_LOCK_FIXTURE(fx, inited);

    fx.PressAtCentre();
    fx.MoveTo(fx.CentreX() + 2.0f);
    fx.Release();

    EXPECT_TRUE(fx.LockEvents.empty());
}

// The drag locks the pointer once it passes the drag threshold and unlocks it
// at the release.
TEST(PointerLockTests, AValueDragLocksThePointerUntilTheRelease)
{
    PointerLockFixture fx;
    const bool inited = fx.Init();
    REQUIRE_POINTER_LOCK_FIXTURE(fx, inited);

    fx.PressAtCentre();
    fx.MoveTo(fx.CentreX() + 10.0f);
    ASSERT_EQ(fx.LockEvents, (std::vector<bool>{true}));

    fx.MoveTo(fx.CentreX() + 20.0f);
    EXPECT_EQ(fx.LockEvents, (std::vector<bool>{true})) << "a held lock is reported once";

    fx.Release();
    EXPECT_EQ(fx.LockEvents, (std::vector<bool>{true, false}));
}

// A locked pointer's position runs on past the window: a move to the left of
// the window's left edge, a negative position, still reaches the drag.
TEST(PointerLockTests, ALockedDragKeepsReadingMovesPastTheWindowEdge)
{
    PointerLockFixture fx;
    const bool inited = fx.Init();
    REQUIRE_POINTER_LOCK_FIXTURE(fx, inited);

    fx.PressAtCentre();
    fx.MoveTo(fx.CentreX() + 10.0f);
    ASSERT_EQ(fx.LockEvents, (std::vector<bool>{true}));
    const float valueInsideWindow = fx.Field->GetValue();

    fx.MoveTo(-400.0f);
    EXPECT_LT(fx.Field->GetValue(), valueInsideWindow)
        << "the move past the window's left edge must still scrub the value";
    EXPECT_LT(fx.Field->GetValue(), 0.0f);

    fx.Release();
    EXPECT_EQ(fx.LockEvents, (std::vector<bool>{true, false}));
}

// The lock belongs to the capture: when the capture ends some other way than
// the release, the lock ends with it.
TEST(PointerLockTests, LosingTheCaptureEndsTheLock)
{
    PointerLockFixture fx;
    const bool inited = fx.Init();
    REQUIRE_POINTER_LOCK_FIXTURE(fx, inited);

    fx.PressAtCentre();
    fx.MoveTo(fx.CentreX() + 10.0f);
    ASSERT_EQ(fx.LockEvents, (std::vector<bool>{true}));

    fx.Ui->ReleaseMouseCapture();
    fx.Pump();
    EXPECT_EQ(fx.LockEvents, (std::vector<bool>{true, false}));

    fx.Release();
}

// Without a capture there is nothing to lock.
TEST(PointerLockTests, LockingWithoutACaptureDoesNothing)
{
    PointerLockFixture fx;
    const bool inited = fx.Init();
    REQUIRE_POINTER_LOCK_FIXTURE(fx, inited);

    fx.Ui->LockCapturedPointer();
    fx.Pump();
    EXPECT_TRUE(fx.LockEvents.empty());
}

// Escape cancels a locked drag: the value goes back to where the drag started,
// the key goes no further, and the lock ends.
TEST(PointerLockTests, EscapeCancelsALockedDragAndRestoresTheStartValue)
{
    PointerLockFixture fx;
    const bool inited = fx.Init();
    REQUIRE_POINTER_LOCK_FIXTURE(fx, inited);

    fx.PressAtCentre();
    fx.MoveTo(fx.CentreX() + 50.0f);
    ASSERT_NE(fx.Field->GetValue(), 1.0f);

    EXPECT_TRUE(fx.Ui->OnKey(Input::kKeyCode_Escape, Input::kKeyActionPress, 0));
    fx.Pump();
    EXPECT_FLOAT_EQ(fx.Field->GetValue(), 1.0f);
    EXPECT_EQ(fx.LockEvents, (std::vector<bool>{true, false}));
    EXPECT_FALSE(fx.Ui->IsMouseCaptured());

    // The held button's release finds no press and changes nothing.
    fx.MoveTo(fx.CentreX() + 80.0f);
    fx.Release();
    EXPECT_FLOAT_EQ(fx.Field->GetValue(), 1.0f);
}

// A press of another button cancels a locked drag the same way, and starts
// nothing of its own.
TEST(PointerLockTests, AnotherButtonCancelsALockedDragAndRestoresTheStartValue)
{
    PointerLockFixture fx;
    const bool inited = fx.Init();
    REQUIRE_POINTER_LOCK_FIXTURE(fx, inited);

    fx.PressAtCentre();
    fx.MoveTo(fx.CentreX() + 50.0f);
    ASSERT_NE(fx.Field->GetValue(), 1.0f);

    constexpr int kRightMouseButton = 1;
    EXPECT_TRUE(fx.Ui->OnMouseButton(kRightMouseButton, true));
    fx.Pump();
    EXPECT_FLOAT_EQ(fx.Field->GetValue(), 1.0f);
    EXPECT_EQ(fx.LockEvents, (std::vector<bool>{true, false}));

    fx.Ui->OnMouseButton(kRightMouseButton, false);
    fx.Release();
    EXPECT_FLOAT_EQ(fx.Field->GetValue(), 1.0f);
}
