#ifndef COMMAND_BUFFER_HPP_INCLUDED
#define COMMAND_BUFFER_HPP_INCLUDED
#include <vector>
#include <memory>

#include "vulkan_include.hh"

#include "logical_device.hh"

#include "effects/effect.hh"
namespace VKIntox
{
    struct LogicalSwapchain;

    std::vector<VkCommandBuffer> allocateCommandBuffer(LogicalDevice* logicalDevice, uint32_t count);

    void writeCommandBuffers(LogicalDevice*                                 logicalDevice,
                             LogicalSwapchain*                              logicalSwapchain,
                             std::vector<std::shared_ptr<VKIntox::Effect>> effects,
                             std::vector<VkCommandBuffer>                   commandBuffers,
                             const DepthState&                              depthState);

    void recordDepthResolveSnapshot(LogicalDevice*            logicalDevice,
                                    LogicalSwapchain*         logicalSwapchain,
                                    VkCommandBuffer           commandBuffer,
                                    uint32_t                  imageIndex,
                                    const DepthState&         depthState);

    std::vector<VkSemaphore> createSemaphores(LogicalDevice* logicalDevice, uint32_t count);
} // namespace VKIntox

#endif // COMMAND_BUFFER_HPP_INCLUDED
