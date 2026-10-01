#ifndef EFFECT_HPP_INCLUDED
#define EFFECT_HPP_INCLUDED
#include <vector>
#include <fstream>
#include <string>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <memory>

#include "vulkan_include.hh"
#include "params/effect_param.hh"

namespace VKIntox
{
    class Effect
    {
    public:
        void virtual applyEffect(uint32_t imageIndex, VkCommandBuffer commandBuffer) = 0;
        void virtual updateEffect(){};
        // Parameter names are omitted on the unused default hooks: subclasses that
        // do consume depth override these, and leaving the names off keeps the
        // base declaration free of unused-parameter noise.
        void virtual useDepthImage(uint32_t /* imageIndex */,
                                   VkImageView /* depthImageView */,
                                   VkImageLayout /* depthImageLayout */ = VK_IMAGE_LAYOUT_UNDEFINED){};
        virtual std::vector<std::unique_ptr<EffectParam>> getParameters() const { return {}; }
        virtual ~Effect() = default;

    private:
    };
} // namespace VKIntox

#endif // EFFECT_HPP_INCLUDED
