#include "effect_fxaa.hh"

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
    FxaaEffect::FxaaEffect(LogicalDevice*       pLogicalDevice,
                           VkFormat             format,
                           VkExtent2D           imageExtent,
                           std::vector<VkImage> inputImages,
                           std::vector<VkImage> outputImages,
                           Config*              pConfig)
    {
        float fxaaQualitySubpix           = pConfig->getOption<float>("fxaaQualitySubpix", 0.75f);
        float fxaaQualityEdgeThreshold    = pConfig->getOption<float>("fxaaQualityEdgeThreshold", 0.125f);
        float fxaaQualityEdgeThresholdMin = pConfig->getOption<float>("fxaaQualityEdgeThresholdMin", 0.0312f);

        vertexCode   = full_screen_triangle_vert;
        fragmentCode = fxaa_frag;

        std::vector<VkSpecializationMapEntry> specMapEntrys(5);

        for (uint32_t i = 0; i < specMapEntrys.size(); i++)
        {
            specMapEntrys[i].constantID = i;
            specMapEntrys[i].offset     = sizeof(float) * i;
            specMapEntrys[i].size       = sizeof(float);
        }
        std::vector<float> specData = {
            fxaaQualitySubpix, fxaaQualityEdgeThreshold, fxaaQualityEdgeThresholdMin, (float) imageExtent.width, (float) imageExtent.height};

        VkSpecializationInfo fragmentSpecializationInfo;
        fragmentSpecializationInfo.mapEntryCount = specMapEntrys.size();
        fragmentSpecializationInfo.pMapEntries   = specMapEntrys.data();
        fragmentSpecializationInfo.dataSize      = sizeof(float) * specData.size();
        fragmentSpecializationInfo.pData         = specData.data();

        pVertexSpecInfo   = nullptr;
        pFragmentSpecInfo = &fragmentSpecializationInfo;

        init(pLogicalDevice, format, imageExtent, inputImages, outputImages, pConfig);
    }
    FxaaEffect::~FxaaEffect()
    {
    }
} // namespace VKIntox
