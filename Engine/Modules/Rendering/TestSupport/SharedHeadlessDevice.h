#pragma once

// The headless Vulkan device a test executable's tests share (SharedHeadlessDevice.cpp).
//
// The device frees a released buffer or texture only at its next BeginFrame or
// WaitForIdle, and most device-backed tests call neither. A UIManager alone holds about
// 113 MB of SDF ring buffers, so the releases of a loop that builds one fixture per case
// stay allocated, as wired GPU memory on a unified-memory Mac, until something retires
// them. Two points do: every test's end (a gtest listener), and the destruction of every
// fixture that declares a SharedDeviceReleaseRetirement.

#include <memory>

namespace GameEngine::Rendering
{
class IDevice;
}

// A headless Vulkan device of the caller's own, with no swapchain and no validation layer.
// Null when no Vulkan device is available: callers GTEST_SKIP on that. For a test that
// asserts on the device itself; every other test takes SharedHeadlessDevice().
std::unique_ptr<GameEngine::Rendering::IDevice> MakeHeadlessDevice();

// The executable's shared headless device: created on first use, shut down after the
// last test, owned by neither caller. A test that asserts nothing about the device itself
// uses it; it never shuts the device down. Null when no Vulkan device is available:
// callers GTEST_SKIP on that.
GameEngine::Rendering::IDevice* SharedHeadlessDevice();

// Idles the shared device and destroys everything released on it so far. A no-op when
// the device was never created.
void RetireSharedDeviceReleases();

// Retires the shared device's releases when it is destroyed. A fixture that builds GPU
// resources on the shared device declares one as its FIRST member: members are destroyed
// in reverse order, so this runs after the fixture's other members have released their
// buffers, and a loop of fixtures inside one test holds one fixture's memory at a time.
struct SharedDeviceReleaseRetirement
{
    ~SharedDeviceReleaseRetirement() { RetireSharedDeviceReleases(); }
};
