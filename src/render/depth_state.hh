#ifndef DEPTH_STATE_HPP_INCLUDED
#define DEPTH_STATE_HPP_INCLUDED

#include "vulkan_include.hh"

namespace VKIntox
{
    struct DepthState
    {
        VkImageView imageView = VK_NULL_HANDLE;
        VkImage image = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent3D extent = {0, 0, 1};
        VkImageLayout observedLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        // Sample count of the source depth image. 1 for non-MSAA, >1 for MSAA
        // depth buffers (e.g. Roblox). The resolve path uses this to decide
        // between copy / resolve / shader-resolve.
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        // True when the source image was created with
        // VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT. Transient images may have
        // restricted lifetime; the layer forces storeOp=STORE and uses the
        // depth-stencil resolve subpass to capture depth before potential
        // discard.
        bool transient = false;
    };
} // namespace VKIntox

#endif // DEPTH_STATE_HPP_INCLUDED
