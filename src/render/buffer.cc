#include "buffer.hh"
#include "memory.hh"

namespace VKIntox
{
    void createBuffer(LogicalDevice*        logicalDevice,
                      VkDeviceSize          size,
                      VkBufferUsageFlags    usage,
                      VkMemoryPropertyFlags properties,
                      VkBuffer&             buffer,
                      VkDeviceMemory&       bufferMemory)
    {
        VkBufferCreateInfo bufferInfo = {};

        bufferInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size        = size;
        bufferInfo.usage       = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkResult result = logicalDevice->vkd.CreateBuffer(logicalDevice->device, &bufferInfo, nullptr, &buffer);
        ASSERT_VULKAN(result);

        VkMemoryRequirements memRequirements;
        logicalDevice->vkd.GetBufferMemoryRequirements(logicalDevice->device, buffer, &memRequirements);

        VkMemoryAllocateInfo allocInfo = {};

        allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize  = memRequirements.size;
        allocInfo.memoryTypeIndex = findMemoryTypeIndex(logicalDevice, memRequirements.memoryTypeBits, properties);

        result = logicalDevice->vkd.AllocateMemory(logicalDevice->device, &allocInfo, nullptr, &bufferMemory);
        ASSERT_VULKAN(result);

        result = logicalDevice->vkd.BindBufferMemory(logicalDevice->device, buffer, bufferMemory, 0);
        ASSERT_VULKAN(result);
    }

} // namespace VKIntox
