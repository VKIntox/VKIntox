#ifndef EFFECT_TRANSFER_HPP_INCLUDED
#define EFFECT_TRANSFER_HPP_INCLUDED
#include <vector>
#include <fstream>
#include <string>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <memory>

#include "render/device/vulkan_include.hh"

#include "effects/runtime/effect.hh"
#include "config/config.hh"

#include "render/device/logical_device.hh"

namespace VKIntox
{
    class TransferEffect : public Effect
    {
    public:
        TransferEffect(LogicalDevice*       logicalDevice,
                       VkFormat             format,
                       VkExtent2D           imageExtent,
                       std::vector<VkImage> inputImages,
                       std::vector<VkImage> outputImages,
                       Config*              config);
        void virtual applyEffect(uint32_t imageIndex, VkCommandBuffer commandBuffer) override;
        virtual ~TransferEffect();

    private:
        LogicalDevice*       logicalDevice;
        std::vector<VkImage> inputImages;
        std::vector<VkImage> outputImages;
        VkExtent2D           imageExtent;
        VkFormat             format;
        Config*              config;
    };
} // namespace VKIntox
#endif // EFFECT_TRANSFER_HPP_INCLUDED
