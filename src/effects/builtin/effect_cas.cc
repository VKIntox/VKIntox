#include "effect_cas.hh"

#include <cstring>

#include "image_view.hh"
#include "descriptor_set.hh"
#include "buffer.hh"
#include "renderpass.hh"
#include "graphics_pipeline.hh"
#include "framebuffer.hh"
#include "shader.hh"
#include "sampler.hh"

#include "shader_sources.hh"

namespace VKIntox
{
    CasEffect::CasEffect(LogicalDevice*       logicalDevice,
                         VkFormat             format,
                         VkExtent2D           imageExtent,
                         std::vector<VkImage> inputImages,
                         std::vector<VkImage> outputImages,
                         Config*              config)
    {

        float sharpness = config->getOption<float>("casSharpness", 0.4f);

        vertexCode   = full_screen_triangle_vert;
        fragmentCode = cas_frag;

        VkSpecializationMapEntry sharpnessMapEntry;
        sharpnessMapEntry.constantID = 0;
        sharpnessMapEntry.offset     = 0;
        sharpnessMapEntry.size       = sizeof(float);

        VkSpecializationInfo fragmentSpecializationInfo;
        fragmentSpecializationInfo.mapEntryCount = 1;
        fragmentSpecializationInfo.pMapEntries   = &sharpnessMapEntry;
        fragmentSpecializationInfo.dataSize      = sizeof(float);
        fragmentSpecializationInfo.pData         = &sharpness;

        vertexSpecInfo   = nullptr;
        fragmentSpecInfo = &fragmentSpecializationInfo;

        init(logicalDevice, format, imageExtent, inputImages, outputImages, config);
    }
    CasEffect::~CasEffect()
    {
    }
} // namespace VKIntox
