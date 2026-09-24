#ifndef MEMORY_HPP_INCLUDED
#define MEMORY_HPP_INCLUDED
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
    uint32_t findMemoryTypeIndex(LogicalDevice* pLogicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties);
}

#endif // MEMORY_HPP_INCLUDED
