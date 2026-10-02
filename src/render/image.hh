#ifndef IMAGE_HPP_INCLUDED
#define IMAGE_HPP_INCLUDED
#include <vector>
#include <fstream>
#include <string>
#include <iostream>
#include <vector>
#include <cstring>
#include <memory>

#include "vulkan_include.hh"

#include "logical_device.hh"

namespace VKIntox
{
    std::vector<VkImage> createImages(LogicalDevice*        logicalDevice,
                                      uint32_t              count,
                                      VkExtent3D            extent,
                                      VkFormat              format,
                                      VkImageUsageFlags     usage,
                                      VkMemoryPropertyFlags properties,
                                      VkDeviceMemory&       imageMemory,
                                      uint32_t              mipLevels = 1);

    // SAFETY: createImages returns an empty vector when any Vulkan call fails, so
    // callers that request exactly one image must not subscript [0] blindly.
    // This returns VK_NULL_HANDLE instead of invoking undefined behaviour.
    VkImage createSingleImage(LogicalDevice*        logicalDevice,
                              VkExtent3D            extent,
                              VkFormat              format,
                              VkImageUsageFlags     usage,
                              VkMemoryPropertyFlags properties,
                              VkDeviceMemory&       imageMemory,
                              uint32_t              mipLevels = 1);

    void uploadToImage(
        LogicalDevice* logicalDevice, VkImage image, VkExtent3D extent, uint32_t size, const unsigned char* writeData, uint32_t mipLevels = 1);

    void changeImageLayout(LogicalDevice* logicalDevice, const std::vector<VkImage>& images, uint32_t mipLevels = 1);

    void generateMipMaps(LogicalDevice* logicalDevice, VkCommandBuffer commandBuffer, VkImage image, VkExtent3D extent, uint32_t mipLevels);
} // namespace VKIntox

#endif // IMAGE_HPP_INCLUDED
