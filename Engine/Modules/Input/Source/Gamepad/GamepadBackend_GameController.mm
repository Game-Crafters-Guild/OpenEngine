// The gamepad backend on macOS: Apple's Game Controller framework. Start loads the framework's
// controller list on a job; the framework posts connect and disconnect on the main thread.
// Manual retain and release, as the Platform module's .mm files: a held controller has one retain.
#include "Gamepad/GamepadSlotTable.h"
#include "Gamepad/SnapshotTripleBuffer.h"
#include "Input/Gamepad/GamepadBackend.h"
#include "Input/GamepadCodes.h"
#include "Input/GamepadFrame.h"
#include "Input/InputScriptApi.h"
#include "Logger/Logger.h"

#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

#include <pthread.h>

#include <cassert>
#include <memory>
#include <mutex>

namespace GameEngine::Input
{
namespace
{

// The most controllers the writer lists. The slots take the first kMaxGamepads; the others wait.
constexpr int kMaxTrackedControllers = 8;

// The connected controllers at one moment. Each listed controller holds one retain.
struct ControllerSnapshot
{
    GCController* Controllers[kMaxTrackedControllers] = {};
    int Count = 0;
};

void ReleaseSnapshot(ControllerSnapshot& snapshot)
{
    for (int i = 0; i < snapshot.Count; ++i)
        [snapshot.Controllers[i] release];
    snapshot = ControllerSnapshot{};
}

void RetainController(GCController* controller)
{
    [controller retain];
}

void ReleaseController(GCController* controller)
{
    [controller release];
}

// The framework reads a stick +Y up, as GamepadAxis does. buttonOptions, buttonHome and the
// stick buttons are nil on a pad without them, and a message to nil reads false.
void StoreExtendedGamepad(GCExtendedGamepad* pad, GamepadFrame::Slot& slot)
{
    slot.connected = true;
    slot.axes[static_cast<int>(GamepadAxis::LeftX)] = pad.leftThumbstick.xAxis.value;
    slot.axes[static_cast<int>(GamepadAxis::LeftY)] = pad.leftThumbstick.yAxis.value;
    slot.axes[static_cast<int>(GamepadAxis::RightX)] = pad.rightThumbstick.xAxis.value;
    slot.axes[static_cast<int>(GamepadAxis::RightY)] = pad.rightThumbstick.yAxis.value;
    slot.axes[static_cast<int>(GamepadAxis::LeftTrigger)] = pad.leftTrigger.value;
    slot.axes[static_cast<int>(GamepadAxis::RightTrigger)] = pad.rightTrigger.value;

    bool* buttons = slot.buttons;
    buttons[static_cast<int>(GamepadButton::A)] = pad.buttonA.pressed;
    buttons[static_cast<int>(GamepadButton::B)] = pad.buttonB.pressed;
    buttons[static_cast<int>(GamepadButton::X)] = pad.buttonX.pressed;
    buttons[static_cast<int>(GamepadButton::Y)] = pad.buttonY.pressed;
    buttons[static_cast<int>(GamepadButton::LeftBumper)] = pad.leftShoulder.pressed;
    buttons[static_cast<int>(GamepadButton::RightBumper)] = pad.rightShoulder.pressed;
    // Menu is the Xbox Menu and the PlayStation Options button; Options is View and Create.
    buttons[static_cast<int>(GamepadButton::Back)] = pad.buttonOptions.pressed;
    buttons[static_cast<int>(GamepadButton::Start)] = pad.buttonMenu.pressed;
    buttons[static_cast<int>(GamepadButton::Guide)] = pad.buttonHome.pressed;
    buttons[static_cast<int>(GamepadButton::LeftThumb)] = pad.leftThumbstickButton.pressed;
    buttons[static_cast<int>(GamepadButton::RightThumb)] = pad.rightThumbstickButton.pressed;
    buttons[static_cast<int>(GamepadButton::DpadUp)] = pad.dpad.up.pressed;
    buttons[static_cast<int>(GamepadButton::DpadRight)] = pad.dpad.right.pressed;
    buttons[static_cast<int>(GamepadButton::DpadDown)] = pad.dpad.down.pressed;
    buttons[static_cast<int>(GamepadButton::DpadLeft)] = pad.dpad.left.pressed;
}

class GameControllerGamepadBackend final : public GamepadBackend
{
  public:
    GamepadRuntimeState Start() override
    {
        @autoreleasepool
        {
            m_Skipped = [[NSMutableSet alloc] init];
            // Observers first, then the attached set: a controller that arrives between the two is
            // in both, and the writer lists it once. With no queue, a handler runs on the posting
            // thread, which the framework documents as the main thread.
            NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
            m_ConnectObserver = [[center addObserverForName:GCControllerDidConnectNotification
                                                     object:nil
                                                      queue:nil
                                                 usingBlock:^(NSNotification* note) {
                                                   OnControllerNotification(note, true);
                                                 }] retain];
            m_DisconnectObserver = [[center addObserverForName:GCControllerDidDisconnectNotification
                                                        object:nil
                                                         queue:nil
                                                    usingBlock:^(NSNotification* note) {
                                                      OnControllerNotification(note, false);
                                                    }] retain];

            // The first call loads the framework's controller list. It runs before the lock, so a
            // handler on the main thread never waits for it.
            [GCController controllers];
            const std::lock_guard<std::mutex> writer(m_WriterMutex);
            NSArray<GCController*>* attached = [GCController controllers];
            for (GCController* controller in attached)
                ApplyControllerStatus(controller, true);
            Logger::Log::Info("Gamepads read through Apple's Game Controller framework ({} attached)",
                              static_cast<unsigned long>(attached.count));
        }
        return GamepadRuntimeState::Available;
    }

    void Read(GamepadFrame& frame) override
    {
        if (m_Snapshots.Acquire())
        {
            const ControllerSnapshot& snapshot = m_Snapshots.ReadBuffer();
            m_Slots.Reconcile(snapshot.Controllers, snapshot.Count, &RetainController, &ReleaseController);
        }

        for (int slot = 0; slot < kMaxGamepads; ++slot)
        {
            GCController* controller = m_Slots.At(slot);
            if (!controller)
                continue;
            // The framework's getters can return autoreleased objects, and the frame thread has no
            // pool of its own between two window event pumps.
            @autoreleasepool
            {
                StoreExtendedGamepad(controller.extendedGamepad, frame.slots[slot]);
            }
        }
    }

    // Runs on the frame thread, the main thread, as the handlers do: no handler runs after the
    // observers are removed.
    void Stop() override
    {
        NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
        for (id* observer : {&m_ConnectObserver, &m_DisconnectObserver})
        {
            if (!*observer)
                continue;
            [center removeObserver:*observer];
            [*observer release];
            *observer = nil;
        }

        // An empty snapshot empties every slot and releases the slots' retains.
        m_Slots.Reconcile(nullptr, 0, &RetainController, &ReleaseController);
        for (int i = 0; i < SnapshotTripleBuffer<ControllerSnapshot>::kBufferCount; ++i)
            ReleaseSnapshot(m_Snapshots.AllBuffers()[i]);
        ReleaseSnapshot(m_WriterControllers);
        [m_Skipped release];
        m_Skipped = nil;
    }

  private:
    void OnControllerNotification(NSNotification* note, bool connected)
    {
        // The handler shares the main thread with Read, so the two never overlap.
        assert(pthread_main_np() != 0 && "Game Controller notification off the main thread");
        @autoreleasepool
        {
            const std::lock_guard<std::mutex> writer(m_WriterMutex);
            ApplyControllerStatus(static_cast<GCController*>(note.object), connected);
        }
    }

    // The writer, under m_WriterMutex: edits its own list, then publishes a copy with its own
    // retains. The mutex orders Start's job against the main thread's handlers; Read never takes it.
    void ApplyControllerStatus(GCController* controller, bool connected)
    {
        int index = 0;
        while (index < m_WriterControllers.Count && m_WriterControllers.Controllers[index] != controller)
            ++index;
        const bool known = index < m_WriterControllers.Count;
        if (connected && !known)
        {
            if (!controller.extendedGamepad)
            {
                NameSkippedController(controller);
                return;
            }
            if (m_WriterControllers.Count == kMaxTrackedControllers)
                return;
            [controller retain];
            m_WriterControllers.Controllers[m_WriterControllers.Count++] = controller;
        }
        else if (!connected && known)
        {
            [m_WriterControllers.Controllers[index] release];
            --m_WriterControllers.Count;
            m_WriterControllers.Controllers[index] = m_WriterControllers.Controllers[m_WriterControllers.Count];
            m_WriterControllers.Controllers[m_WriterControllers.Count] = nil;
        }
        else
        {
            if (!connected)
                [m_Skipped removeObject:controller];
            return;
        }

        ControllerSnapshot& back = m_Snapshots.WriteBuffer();
        ReleaseSnapshot(back);
        for (int i = 0; i < m_WriterControllers.Count; ++i)
        {
            [m_WriterControllers.Controllers[i] retain];
            back.Controllers[i] = m_WriterControllers.Controllers[i];
        }
        back.Count = m_WriterControllers.Count;
        m_Snapshots.Publish();
    }

    // Names a controller without the extended gamepad profile once while it stays connected.
    void NameSkippedController(GCController* controller)
    {
        if ([m_Skipped containsObject:controller])
            return;
        [m_Skipped addObject:controller];
        NSString* name = controller.vendorName;
        Logger::Log::Warning("Game controller \"{}\" has no extended gamepad profile, so it does not read as a gamepad.",
                             name ? name.UTF8String : "unnamed");
    }

    id m_ConnectObserver = nil;
    id m_DisconnectObserver = nil;
    std::mutex m_WriterMutex;
    // The writer's state, under m_WriterMutex: the controllers it lists and those it named as skipped.
    ControllerSnapshot m_WriterControllers;
    NSMutableSet* m_Skipped = nil;
    SnapshotTripleBuffer<ControllerSnapshot> m_Snapshots;
    GamepadSlotTable<GCController> m_Slots;
};

} // namespace

std::unique_ptr<GamepadBackend> CreatePlatformGamepadBackend()
{
    return std::make_unique<GameControllerGamepadBackend>();
}

} // namespace GameEngine::Input
