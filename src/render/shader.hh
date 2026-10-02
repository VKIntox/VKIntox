#ifndef SHADER_HPP_INCLUDED
#define SHADER_HPP_INCLUDED
#include <vector>
#include <fstream>
#include <string>
#include <iostream>
#include <vector>
#include <cstdlib>
#include <memory>

#include "vulkan_include.hh"

#include "logical_device.hh"

namespace VKIntox
{
    void createShaderModule(LogicalDevice* logicalDevice, const std::vector<char>& code, VkShaderModule* shaderModule);
    void createShaderModule(LogicalDevice* logicalDevice, const std::vector<uint32_t>& code, VkShaderModule* shaderModule);
} // namespace VKIntox

#endif // SHADER_HPP_INCLUDED
