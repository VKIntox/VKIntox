#pragma once
#include <vector>
#include <fstream>
#include <string>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <memory>

#include "vulkan_include.hh"

#include "../effect_simple.hh"
#include "config.hh"

namespace VKIntox
{
    class DlsEffect : public SimpleEffect
    {
    public:
        DlsEffect(LogicalDevice*       pLogicalDevice,
                  VkFormat             format,
                  VkExtent2D           imageExtent,
                  std::vector<VkImage> inputImages,
                  std::vector<VkImage> outputImages,
                  Config*              pConfig);
        ~DlsEffect();
    };
} // namespace VKIntox
