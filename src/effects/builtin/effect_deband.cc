#include "effect_deband.hh"

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
    DebandEffect::DebandEffect(LogicalDevice*       logicalDevice,
                               VkFormat             format,
                               VkExtent2D           imageExtent,
                               std::vector<VkImage> inputImages,
                               std::vector<VkImage> outputImages,
                               Config*              config)
    {
        vertexCode   = full_screen_triangle_vert;
        fragmentCode = deband_frag;

        struct
        {
            float   screenWidth;
            float   screenHeight;
            float   reverseScreenWidth;
            float   reverseScreenHeight;
            float   debandAvgdiff;
            float   debandMaxdiff;
            float   debandMiddiff;
            float   range;
            int32_t iterations;
        } debandOptions{};

        debandOptions.screenWidth         = (float) imageExtent.width;
        debandOptions.screenHeight        = (float) imageExtent.height;
        debandOptions.reverseScreenWidth  = 1.0f / imageExtent.width;
        debandOptions.reverseScreenHeight = 1.0f / imageExtent.height;

        // get Options
        debandOptions.debandAvgdiff = config->getOption<float>("debandAvgdiff", 3.4f);
        debandOptions.debandMaxdiff = config->getOption<float>("debandMaxdiff", 6.8f);
        debandOptions.debandMiddiff = config->getOption<float>("debandMiddiff", 3.3f);
        debandOptions.range         = config->getOption<float>("debandRange", 16.0f);
        debandOptions.iterations    = config->getOption<int32_t>("debandIterations", 4);

        std::vector<VkSpecializationMapEntry> specMapEntrys(9);
        for (uint32_t i = 0; i < specMapEntrys.size(); i++)
        {
            specMapEntrys[i].constantID = i;
            specMapEntrys[i].offset     = sizeof(float) * i; // TODO not clean to assume that sizeof(int32_t) == sizeof(float)
            specMapEntrys[i].size       = sizeof(float);
        }

        VkSpecializationInfo specializationInfo;
        specializationInfo.mapEntryCount = specMapEntrys.size();
        specializationInfo.pMapEntries   = specMapEntrys.data();
        specializationInfo.dataSize      = sizeof(debandOptions);
        specializationInfo.pData         = &debandOptions;

        vertexSpecInfo   = nullptr;
        fragmentSpecInfo = &specializationInfo;

        init(logicalDevice, format, imageExtent, inputImages, outputImages, config);
    }
    DebandEffect::~DebandEffect()
    {
    }
} // namespace VKIntox
