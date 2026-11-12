// GameInput: the gamepad backend on Windows. The start job creates GameInput, parses the mapping
// database and registers the device callbacks. The frame reads the latest device snapshot and one
// reading per slot, without a lock.
#include "Gamepad/ControllerMapping/ControllerMapping.h"
#include "Gamepad/ControllerMapping/ControllerMappingDatabase.h"
#include "Gamepad/GameInputReading.h"
#include "Gamepad/GamepadSlotTable.h"
#include "Gamepad/SnapshotTripleBuffer.h"
#include "Input/Gamepad/GamepadBackend.h"
#include "Input/GamepadCodes.h"
#include "Input/GamepadFrame.h"
#include "Logger/Logger.h"
#include "Types/FlatSet.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <gameinput.h>

namespace GameEngine::Input
{
namespace
{

using GameInput::v3::IGameInput;
using GameInput::v3::IGameInputDevice;
using GameInput::v3::IGameInputReading;

// The most gamepad devices the snapshot tracks; slots beyond kMaxGamepads wait for a free slot.
constexpr int kMaxTrackedDevices = 8;

// The most raw controls a controller reading keeps; controls past these read released or at rest.
constexpr std::uint32_t kMaxControllerAxes = 32;
constexpr std::uint32_t kMaxControllerButtons = 128;
constexpr std::uint32_t kMaxControllerSwitches = 4;

// The mapping database's name for this platform's entries.
constexpr std::string_view kMappingPlatform = "Windows";

constexpr char kRuntimeFix[] =
    "Install the GameInput runtime from Microsoft, then restart the game to use a gamepad (winget install "
    "Microsoft.GameInput).";
constexpr char kFailedFix[] =
    "GameInput is installed but did not start. If this repeats, reinstall it (winget install Microsoft.GameInput).";

// The connected gamepads at one moment. Each listed device holds one reference. A device with a
// mapping reads as a controller through its database entry; one without reads as a gamepad.
struct DeviceSnapshot
{
    IGameInputDevice* Devices[kMaxTrackedDevices] = {};
    const ControllerMapping* Mappings[kMaxTrackedDevices] = {};
    int Count = 0;
};

void ReleaseSnapshot(DeviceSnapshot& snapshot)
{
    for (int i = 0; i < snapshot.Count; ++i)
        snapshot.Devices[i]->Release();
    snapshot = DeviceSnapshot{};
}

// The loader in gameinput.lib reports these when the runtime is absent or too old for API v3.
bool IsMissingRuntime(HRESULT result)
{
    return result == HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND) || result == HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND) ||
           result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || result == E_NOINTERFACE ||
           result == REGDB_E_CLASSNOTREG;
}

// The raw controls of a controller reading, mapped through the device's database entry.
void StoreControllerState(IGameInputReading& reading, const ControllerMapping& mapping, GamepadFrame::Slot& slot)
{
    float axes[kMaxControllerAxes] = {};
    bool buttons[kMaxControllerButtons] = {};
    GameInput::v3::GameInputSwitchPosition switches[kMaxControllerSwitches] = {};
    std::uint8_t hats[kMaxControllerSwitches] = {};
    // A count never exceeds its array, whatever GameInput returns.
    const std::uint32_t axisCount =
        std::min(reading.GetControllerAxisState(kMaxControllerAxes, axes), kMaxControllerAxes);
    const std::uint32_t buttonCount =
        std::min(reading.GetControllerButtonState(kMaxControllerButtons, buttons), kMaxControllerButtons);
    const std::uint32_t switchCount =
        std::min(reading.GetControllerSwitchState(kMaxControllerSwitches, switches), kMaxControllerSwitches);
    // GameInput reads a controller axis 0 to 1; the database reads it -1 to 1.
    for (std::uint32_t i = 0; i < axisCount; ++i)
        axes[i] = axes[i] * 2.0f - 1.0f;
    for (std::uint32_t i = 0; i < switchCount; ++i)
        hats[i] = ToHatMask(switches[i]);
    mapping.Apply({{axes, axisCount}, {buttons, buttonCount}, {hats, switchCount}}, slot);
}

std::uint32_t DeviceKey(const GameInput::v3::GameInputDeviceInfo& info)
{
    return (static_cast<std::uint32_t>(info.vendorId) << 16) | info.productId;
}

void AddDeviceReference(IGameInputDevice* device)
{
    device->AddRef();
}

void ReleaseDeviceReference(IGameInputDevice* device)
{
    device->Release();
}

class GameInputGamepadBackend final : public GamepadBackend
{
  public:
    GamepadRuntimeState Start() override
    {
        const HRESULT created = GameInput::v3::GameInputCreate(&m_GameInput);
        if (FAILED(created))
        {
            m_GameInput = nullptr;
            const bool missing = IsMissingRuntime(created);
            const char* fix = missing ? kRuntimeFix : kFailedFix;
            Logger::Log::Error("Gamepads are off: GameInputCreate failed (0x{:08X}). {}",
                               static_cast<std::uint32_t>(created), fix);
            return missing ? GamepadRuntimeState::RuntimeMissing : GamepadRuntimeState::Failed;
        }

        // The device callbacks read the database, so the parse comes first.
        LoadMappings();

        // Async enumeration: the devices present now arrive on GameInput's thread too.
        const HRESULT registered = RegisterDeviceCallback(GameInput::v3::GameInputKindGamepad, m_GamepadCallbackToken);
        if (FAILED(registered))
        {
            Logger::Log::Error("Gamepads are off: GameInput refused the device callback (0x{:08X}).",
                               static_cast<std::uint32_t>(registered));
            return GamepadRuntimeState::Failed;
        }
        // The controller kind brings the pads that GameInput does not read as gamepads.
        const HRESULT controllerRegistered =
            RegisterDeviceCallback(GameInput::v3::GameInputKindController, m_ControllerCallbackToken);
        if (FAILED(controllerRegistered))
            Logger::Log::Error("Only Xbox-class gamepads work: GameInput refused the controller callback (0x{:08X}).",
                               static_cast<std::uint32_t>(controllerRegistered));
        Logger::Log::Info("Gamepads read through Microsoft GameInput");
        return GamepadRuntimeState::Available;
    }

    void Read(GamepadFrame& frame) override
    {
        if (m_Snapshots.Acquire())
        {
            const DeviceSnapshot& snapshot = m_Snapshots.ReadBuffer();
            m_Slots.Reconcile(snapshot.Devices, snapshot.Count, &AddDeviceReference, &ReleaseDeviceReference);
        }

        for (int slot = 0; slot < kMaxGamepads; ++slot)
        {
            IGameInputDevice* device = m_Slots.At(slot);
            if (!device)
                continue;
            const ControllerMapping* mapping = FindMapping(m_Snapshots.ReadBuffer(), device);
            const GameInput::v3::GameInputKind kind =
                mapping ? GameInput::v3::GameInputKindController : GameInput::v3::GameInputKindGamepad;
            // A device removed since the snapshot fails here and reads as disconnected this frame.
            IGameInputReading* reading = nullptr;
            if (FAILED(m_GameInput->GetCurrentReading(kind, device, &reading)))
                continue;
            if (mapping)
            {
                StoreControllerState(*reading, *mapping, frame.slots[slot]);
            }
            else
            {
                GameInput::v3::GameInputGamepadState state{};
                if (reading->GetGamepadState(&state))
                    StoreGamepadState(state, frame.slots[slot]);
            }
            reading->Release();
        }
    }

    void Stop() override
    {
        if (!m_GameInput)
            return;
        UnregisterDeviceCallback(m_GamepadCallbackToken);
        UnregisterDeviceCallback(m_ControllerCallbackToken);

        // An empty snapshot empties every slot and releases the slots' references.
        m_Slots.Reconcile(nullptr, 0, &AddDeviceReference, &ReleaseDeviceReference);
        for (int i = 0; i < SnapshotTripleBuffer<DeviceSnapshot>::kBufferCount; ++i)
            ReleaseSnapshot(m_Snapshots.AllBuffers()[i]);
        ReleaseSnapshot(m_WriterDevices);
        m_GameInput->Release();
        m_GameInput = nullptr;
    }

  private:
    // Runs on the start job, before any device callback can read the database.
    void LoadMappings()
    {
        const auto started = std::chrono::steady_clock::now();
        m_Mappings = ControllerMappingDatabase::ParseShipped(kMappingPlatform);
        const std::chrono::duration<double, std::milli> elapsed = std::chrono::steady_clock::now() - started;
        for (const ControllerMappingDatabase::RefusedLine& refused : m_Mappings.RefusedLines())
            Logger::Log::Warning("Controller mapping database line {} is skipped: {}", refused.LineNumber,
                                 refused.Reason);
        Logger::Log::Info("Controller mapping database: {} controllers, parsed in {:.1f} ms on a job",
                          m_Mappings.EntryCount(), elapsed.count());
    }

    HRESULT RegisterDeviceCallback(GameInput::v3::GameInputKind kind, GameInput::v3::GameInputCallbackToken& token)
    {
        const HRESULT result = m_GameInput->RegisterDeviceCallback(
            nullptr, kind, GameInput::v3::GameInputDeviceConnected, GameInput::v3::GameInputAsyncEnumeration, this,
            &GameInputGamepadBackend::OnDeviceStatus, &token);
        if (FAILED(result))
            token = 0;
        return result;
    }

    // UnregisterCallback returns true after any running callback; no writer runs after that.
    void UnregisterDeviceCallback(GameInput::v3::GameInputCallbackToken& token)
    {
        if (token != 0 && !m_GameInput->UnregisterCallback(token))
            Logger::Log::Error("GameInput did not unregister a gamepad device callback at shutdown");
        token = 0;
    }

    static const ControllerMapping* FindMapping(const DeviceSnapshot& snapshot, const IGameInputDevice* device)
    {
        for (int i = 0; i < snapshot.Count; ++i)
        {
            if (snapshot.Devices[i] == device)
                return snapshot.Mappings[i];
        }
        return nullptr;
    }

    // How an arriving device reads: as a gamepad (mapping stays nullptr), or as a controller
    // through its database entry. @return False for a controller with no entry; it is named once.
    bool ClassifyDevice(IGameInputDevice* device, const ControllerMapping*& mapping)
    {
        mapping = nullptr;
        const GameInput::v3::GameInputDeviceInfo* info = nullptr;
        if (FAILED(device->GetDeviceInfo(&info)) || !info)
            return false;
        // Both callbacks report an Xbox-class pad; it reads as a gamepad.
        if ((info->supportedInput & GameInput::v3::GameInputKindGamepad) != 0)
            return true;
        mapping = m_Mappings.Find(info->vendorId, info->productId);
        if (mapping)
            return true;
        if (m_NamedDevices.Insert(DeviceKey(*info)))
            Logger::Log::Info("The controller '{}' (vendor 0x{:04X}, product 0x{:04X}) has no entry in the controller "
                              "mapping database, so it does not read as a gamepad",
                              info->displayName ? info->displayName : "unnamed", info->vendorId, info->productId);
        return false;
    }

    static void CALLBACK OnDeviceStatus(GameInput::v3::GameInputCallbackToken, void* context, IGameInputDevice* device,
                                        std::uint64_t, GameInput::v3::GameInputDeviceStatus currentStatus,
                                        GameInput::v3::GameInputDeviceStatus)
    {
        static_cast<GameInputGamepadBackend*>(context)->ApplyDeviceStatus(
            device, (currentStatus & GameInput::v3::GameInputDeviceConnected) != 0);
    }

    // The single writer: edits its own set, then publishes a copy with its own references.
    void ApplyDeviceStatus(IGameInputDevice* device, bool connected)
    {
        // GameInput does not document that device callbacks never overlap: the mutex keeps one
        // writer. Only device events take it; the frame read never does.
        const std::lock_guard<std::mutex> writer(m_WriterMutex);
        int index = 0;
        while (index < m_WriterDevices.Count && m_WriterDevices.Devices[index] != device)
            ++index;
        const bool known = index < m_WriterDevices.Count;
        const ControllerMapping* mapping = nullptr;
        if (connected && !known && m_WriterDevices.Count < kMaxTrackedDevices && ClassifyDevice(device, mapping))
        {
            device->AddRef();
            m_WriterDevices.Devices[m_WriterDevices.Count] = device;
            m_WriterDevices.Mappings[m_WriterDevices.Count] = mapping;
            ++m_WriterDevices.Count;
        }
        else if (!connected && known)
        {
            m_WriterDevices.Devices[index]->Release();
            --m_WriterDevices.Count;
            m_WriterDevices.Devices[index] = m_WriterDevices.Devices[m_WriterDevices.Count];
            m_WriterDevices.Mappings[index] = m_WriterDevices.Mappings[m_WriterDevices.Count];
            m_WriterDevices.Devices[m_WriterDevices.Count] = nullptr;
            m_WriterDevices.Mappings[m_WriterDevices.Count] = nullptr;
        }
        else
        {
            return;
        }

        DeviceSnapshot& back = m_Snapshots.WriteBuffer();
        ReleaseSnapshot(back);
        for (int i = 0; i < m_WriterDevices.Count; ++i)
        {
            m_WriterDevices.Devices[i]->AddRef();
            back.Devices[i] = m_WriterDevices.Devices[i];
            back.Mappings[i] = m_WriterDevices.Mappings[i];
        }
        back.Count = m_WriterDevices.Count;
        m_Snapshots.Publish();
    }

    IGameInput* m_GameInput = nullptr;
    GameInput::v3::GameInputCallbackToken m_GamepadCallbackToken = 0;
    GameInput::v3::GameInputCallbackToken m_ControllerCallbackToken = 0;
    // Written once in Start, before the callbacks register; read-only after that.
    ControllerMappingDatabase m_Mappings;
    std::mutex m_WriterMutex;
    // The writer's state, under m_WriterMutex: the devices it lists and the controllers it named.
    DeviceSnapshot m_WriterDevices;
    FlatSet<std::uint32_t> m_NamedDevices;
    SnapshotTripleBuffer<DeviceSnapshot> m_Snapshots;
    GamepadSlotTable<IGameInputDevice> m_Slots;
};

} // namespace

std::unique_ptr<GamepadBackend> CreatePlatformGamepadBackend()
{
    return std::make_unique<GameInputGamepadBackend>();
}

} // namespace GameEngine::Input
