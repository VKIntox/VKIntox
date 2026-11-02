#ifndef BUFFER_HPP_INCLUDED
#define BUFFER_HPP_INCLUDED
#include <vector>
#include <fstream>
#include <string>
#include <iostream>
#include <vector>
#include <memory>

#include "render/device/vulkan_include.hh"

#include "render/device/logical_device.hh"

namespace VKIntox
{
    void createBuffer(LogicalDevice*        logicalDevice,
                      VkDeviceSize          size,
                      VkBufferUsageFlags    usage,
                      VkMemoryPropertyFlags properties,
                      VkBuffer&             buffer,
                      VkDeviceMemory&       bufferMemory);
}

#endif // BUFFER_HPP_INCLUDED
