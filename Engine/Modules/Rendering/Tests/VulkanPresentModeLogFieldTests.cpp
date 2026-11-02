#include <gtest/gtest.h>

#include "Vulkan/VulkanMappings.h"

using namespace GameEngine::Rendering;

// A timing window reads the present mode back from the swapchain Info log line,
// so each mode the swapchain can use has its own name and pacing in the field.
TEST(VulkanPresentModeLogField, NamesEachModeAndItsPacing)
{
    EXPECT_EQ(VulkanMappings::PresentModeLogField(VK_PRESENT_MODE_IMMEDIATE_KHR),
              "presentMode=IMMEDIATE displayPaced=false");
    EXPECT_EQ(VulkanMappings::PresentModeLogField(VK_PRESENT_MODE_MAILBOX_KHR),
              "presentMode=MAILBOX displayPaced=false");
    EXPECT_EQ(VulkanMappings::PresentModeLogField(VK_PRESENT_MODE_FIFO_KHR), "presentMode=FIFO displayPaced=true");
    EXPECT_EQ(VulkanMappings::PresentModeLogField(VK_PRESENT_MODE_FIFO_RELAXED_KHR),
              "presentMode=FIFO_RELAXED displayPaced=true");
}

// A mode the engine does not name is reported as unknown, never as a named mode.
TEST(VulkanPresentModeLogField, ReportsAnUnnamedModeAsUnknown)
{
    EXPECT_EQ(VulkanMappings::PresentModeLogField(VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR),
              "presentMode=UNKNOWN displayPaced=unknown");
}
