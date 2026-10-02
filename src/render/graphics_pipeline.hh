#ifndef GRAPHICS_PIPELINE_HPP_INCLUDED
#define GRAPHICS_PIPELINE_HPP_INCLUDED
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
    VkPipelineLayout createGraphicsPipelineLayout(LogicalDevice* logicalDevice, 
                                                   std::vector<VkDescriptorSetLayout> descriptorSetLayouts,
                                                   std::vector<VkPushConstantRange> pushConstantRanges = {});

    VkPipeline createGraphicsPipeline(LogicalDevice*        logicalDevice,
                                      VkShaderModule        vertexModule,
                                      VkSpecializationInfo* vertexSpecializationInfo,
                                      std::string           vertexEntryPoint,
                                      VkShaderModule        fragmentModule,
                                      VkSpecializationInfo* fragmentSpecializationInfo,
                                      std::string           fragmentEntryPoint,
                                      VkExtent2D            extent,
                                      VkRenderPass          renderPass,
                                      VkPipelineLayout      pipelineLayout,
                                      bool                  flip = false);

} // namespace VKIntox

#endif // GRAPHICS_PIPELINE_HPP_INCLUDED
