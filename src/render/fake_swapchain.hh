#ifndef FAKE_SWAPCHAIN_HPP_INCLUDED
#define FAKE_SWAPCHAIN_HPP_INCLUDED
#include <vector>
#include <fstream>
#include <string>
#include <iostream>
#include <vector>
#include <memory>

#include "vulkan_include.hh"

#include "logical_device.hh"

namespace VKIntox
{
    std::vector<VkImage> createFakeSwapchainImages(LogicalDevice*                    pLogicalDevice,
                                                      VkSwapchainCreateInfoKHR          swapchainCreateInfo,
                                                      uint32_t                          count,
                                                      std::vector<VkDeviceMemory>&      deviceMemories);
}

#endif // FAKE_SWAPCHAIN_HPP_INCLUDED
