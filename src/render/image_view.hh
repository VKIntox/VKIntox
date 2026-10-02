#ifndef IMAGE_VIEW_HPP_INCLUDED
#define IMAGE_VIEW_HPP_INCLUDED
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
    std::vector<VkImageView> createImageViews(LogicalDevice*       logicalDevice,
                                              VkFormat             format,
                                              const std::vector<VkImage>& images,
                                              VkImageViewType      viewType   = VK_IMAGE_VIEW_TYPE_2D,
                                              VkImageAspectFlags   aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                              uint32_t             mipLevels  = 1);

    // SAFETY: createImageViews returns an empty vector when any Vulkan call fails,
    // so callers that request exactly one view must not subscript [0] blindly.
    // This returns VK_NULL_HANDLE instead of invoking undefined behaviour.
    VkImageView createSingleImageView(LogicalDevice*     logicalDevice,
                                      VkFormat           format,
                                      VkImage            image,
                                      VkImageViewType    viewType   = VK_IMAGE_VIEW_TYPE_2D,
                                      VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                      uint32_t           mipLevels  = 1);
}

#endif // IMAGE_VIEW_HPP_INCLUDED
