#include "renderpass.hh"

namespace VKIntox
{
    VkRenderPass createRenderPass(LogicalDevice* logicalDevice, VkFormat format, VkImageLayout finalLayout)
    {
        VkRenderPass renderPass;

        VkAttachmentDescription attachmentDescription;
        attachmentDescription.flags          = 0;
        attachmentDescription.format         = format;
        attachmentDescription.samples        = VK_SAMPLE_COUNT_1_BIT;
        attachmentDescription.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachmentDescription.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        attachmentDescription.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachmentDescription.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachmentDescription.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        attachmentDescription.finalLayout    = finalLayout;

        VkAttachmentReference attachmentReference;
        attachmentReference.attachment = 0;
        attachmentReference.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpassDescription;
        subpassDescription.flags                   = 0;
        subpassDescription.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpassDescription.inputAttachmentCount    = 0;
        subpassDescription.pInputAttachments       = nullptr;
        subpassDescription.colorAttachmentCount    = 1;
        subpassDescription.pColorAttachments       = &attachmentReference;
        subpassDescription.pResolveAttachments     = nullptr;
        subpassDescription.pDepthStencilAttachment = nullptr;
        subpassDescription.preserveAttachmentCount = 0;
        subpassDescription.pPreserveAttachments    = nullptr;

        VkSubpassDependency subpassDependencies[2];

        // Incoming: wait for prior color writes before this render pass
        subpassDependencies[0].srcSubpass      = VK_SUBPASS_EXTERNAL;
        subpassDependencies[0].dstSubpass      = 0;
        subpassDependencies[0].srcStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        subpassDependencies[0].dstStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        subpassDependencies[0].srcAccessMask   = 0;
        subpassDependencies[0].dstAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        subpassDependencies[0].dependencyFlags = 0;

        // Outgoing: ensure writes + layout transition are visible before subsequent reads
        subpassDependencies[1].srcSubpass      = 0;
        subpassDependencies[1].dstSubpass      = VK_SUBPASS_EXTERNAL;
        subpassDependencies[1].srcStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        subpassDependencies[1].dstStageMask    = (finalLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                                                      ? (VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)
                                                      : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        subpassDependencies[1].srcAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        subpassDependencies[1].dstAccessMask   = (finalLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                                                      ? VK_ACCESS_SHADER_READ_BIT
                                                      : VK_ACCESS_MEMORY_READ_BIT;
        subpassDependencies[1].dependencyFlags = 0;

        VkRenderPassCreateInfo renderPassCreateInfo;
        renderPassCreateInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassCreateInfo.pNext           = nullptr;
        renderPassCreateInfo.flags           = 0;
        renderPassCreateInfo.attachmentCount = 1;
        renderPassCreateInfo.pAttachments    = &attachmentDescription;
        renderPassCreateInfo.subpassCount    = 1;
        renderPassCreateInfo.pSubpasses      = &subpassDescription;
        renderPassCreateInfo.dependencyCount = 2;
        renderPassCreateInfo.pDependencies   = subpassDependencies;

        VkResult result = logicalDevice->vkd.CreateRenderPass(logicalDevice->device, &renderPassCreateInfo, nullptr, &renderPass);
        ASSERT_VULKAN_VAL(result, VK_NULL_HANDLE);

        return renderPass;
    }

    VkRenderPass createDepthMsaaResolveRenderPass(LogicalDevice*         logicalDevice,
                                                  VkFormat               depthFormat,
                                                  VkSampleCountFlagBits  samples,
                                                  VkResolveModeFlagBits  depthResolveMode,
                                                  VkImageLayout          resolveFinalLayout)
    {
        if (samples == VK_SAMPLE_COUNT_1_BIT || depthResolveMode == 0)
            return VK_NULL_HANDLE;

        // Attachment 0: multisampled depth source (input). Loaded, discarded
        // after (the game owns its contents).
        VkAttachmentDescription2 attachments[2];
        attachments[0].sType          = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
        attachments[0].pNext          = nullptr;
        attachments[0].flags          = 0;
        attachments[0].format         = depthFormat;
        attachments[0].samples        = samples;
        attachments[0].loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachments[0].storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].initialLayout  = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachments[0].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        // Attachment 1: 1-sample resolve target that effects will sample.
        attachments[1].sType          = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
        attachments[1].pNext          = nullptr;
        attachments[1].flags          = 0;
        attachments[1].format         = depthFormat;
        attachments[1].samples        = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[1].finalLayout    = resolveFinalLayout;

        VkAttachmentReference2 depthInputRef;
        depthInputRef.sType       = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
        depthInputRef.pNext       = nullptr;
        depthInputRef.attachment  = 0;
        depthInputRef.layout      = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthInputRef.aspectMask  = VK_IMAGE_ASPECT_DEPTH_BIT;

        VkAttachmentReference2 depthResolveRef;
        depthResolveRef.sType      = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
        depthResolveRef.pNext      = nullptr;
        depthResolveRef.attachment = 1;
        depthResolveRef.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthResolveRef.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;

        VkSubpassDescriptionDepthStencilResolve depthResolve;
        depthResolve.sType            = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE;
        depthResolve.pNext            = nullptr;
        depthResolve.depthResolveMode = depthResolveMode;
        depthResolve.stencilResolveMode = VK_RESOLVE_MODE_NONE;
        depthResolve.pDepthStencilResolveAttachment = &depthResolveRef;

        VkSubpassDescription2 subpassDescription;
        subpassDescription.sType                   = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
        subpassDescription.pNext                   = &depthResolve;
        subpassDescription.flags                   = 0;
        subpassDescription.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpassDescription.viewMask                = 0;
        subpassDescription.inputAttachmentCount    = 0;
        subpassDescription.pInputAttachments       = nullptr;
        subpassDescription.colorAttachmentCount    = 0;
        subpassDescription.pColorAttachments       = nullptr;
        subpassDescription.pResolveAttachments     = nullptr;
        subpassDescription.pDepthStencilAttachment = &depthInputRef;
        subpassDescription.preserveAttachmentCount = 0;
        subpassDescription.pPreserveAttachments    = nullptr;

        VkSubpassDependency2 subpassDependencies[2];

        // Wait for the game's depth writes to complete before resolving.
        subpassDependencies[0].sType           = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
        subpassDependencies[0].pNext           = nullptr;
        subpassDependencies[0].srcSubpass      = VK_SUBPASS_EXTERNAL;
        subpassDependencies[0].dstSubpass      = 0;
        subpassDependencies[0].srcStageMask    = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        subpassDependencies[0].dstStageMask    = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        subpassDependencies[0].srcAccessMask   = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        subpassDependencies[0].dstAccessMask   = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        subpassDependencies[0].dependencyFlags = 0;
        subpassDependencies[0].viewOffset      = 0;

        // Make the resolved depth visible to effects (shader read).
        subpassDependencies[1].sType           = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
        subpassDependencies[1].pNext           = nullptr;
        subpassDependencies[1].srcSubpass      = 0;
        subpassDependencies[1].dstSubpass      = VK_SUBPASS_EXTERNAL;
        subpassDependencies[1].srcStageMask    = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        subpassDependencies[1].dstStageMask    = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        subpassDependencies[1].srcAccessMask   = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        subpassDependencies[1].dstAccessMask   = VK_ACCESS_SHADER_READ_BIT;
        subpassDependencies[1].dependencyFlags = 0;
        subpassDependencies[1].viewOffset      = 0;

        VkRenderPassCreateInfo2 renderPassCreateInfo;
        renderPassCreateInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2;
        renderPassCreateInfo.pNext           = nullptr;
        renderPassCreateInfo.flags           = 0;
        renderPassCreateInfo.attachmentCount = 2;
        renderPassCreateInfo.pAttachments    = attachments;
        renderPassCreateInfo.subpassCount    = 1;
        renderPassCreateInfo.pSubpasses      = &subpassDescription;
        renderPassCreateInfo.dependencyCount = 2;
        renderPassCreateInfo.pDependencies   = subpassDependencies;
        renderPassCreateInfo.correlatedViewMaskCount = 0;
        renderPassCreateInfo.pCorrelatedViewMasks    = nullptr;

        VkRenderPass renderPass = VK_NULL_HANDLE;
        VkResult result = logicalDevice->vkd.CreateRenderPass2(logicalDevice->device, &renderPassCreateInfo, nullptr, &renderPass);
        ASSERT_VULKAN_VAL(result, VK_NULL_HANDLE);

        return renderPass;
    }

    VkRenderPass createDepthMsaaTransformRenderPass(LogicalDevice*         logicalDevice,
                                                    VkFormat               depthFormat,
                                                    VkSampleCountFlagBits  samples,
                                                    VkResolveModeFlagBits  depthResolveMode)
    {
        if (samples == VK_SAMPLE_COUNT_1_BIT || depthResolveMode == 0)
            return VK_NULL_HANDLE;

        // Attachment 0: multisampled depth source (input). Loaded, discarded
        // after (the game owns its contents).
        VkAttachmentDescription2 attachments[3];
        attachments[0].sType          = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
        attachments[0].pNext          = nullptr;
        attachments[0].flags          = 0;
        attachments[0].format         = depthFormat;
        attachments[0].samples        = samples;
        attachments[0].loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachments[0].storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].initialLayout  = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachments[0].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        // Attachment 1: 1-sample resolve target that subpass 1 samples. It ends
        // in GENERAL, the layout the shared descriptor writer bakes.
        attachments[1].sType          = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
        attachments[1].pNext          = nullptr;
        attachments[1].flags          = 0;
        attachments[1].format         = depthFormat;
        attachments[1].samples        = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[1].finalLayout    = VK_IMAGE_LAYOUT_GENERAL;

        // Attachment 2: r32 image the effects sample after the pass.
        attachments[2].sType          = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
        attachments[2].pNext          = nullptr;
        attachments[2].flags          = 0;
        attachments[2].format         = VK_FORMAT_R32_SFLOAT;
        attachments[2].samples        = VK_SAMPLE_COUNT_1_BIT;
        attachments[2].loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[2].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[2].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[2].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[2].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference2 depthInputRef;
        depthInputRef.sType       = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
        depthInputRef.pNext       = nullptr;
        depthInputRef.attachment  = 0;
        depthInputRef.layout      = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthInputRef.aspectMask  = VK_IMAGE_ASPECT_DEPTH_BIT;

        VkAttachmentReference2 depthResolveRef;
        depthResolveRef.sType      = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
        depthResolveRef.pNext      = nullptr;
        depthResolveRef.attachment = 1;
        depthResolveRef.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthResolveRef.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;

        // referencing the intermediate in subpass 1 pins its layout for the
        // sampling draw instead of leaving it to final-layout timing.
        VkAttachmentReference2 resolvedInputRef;
        resolvedInputRef.sType      = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
        resolvedInputRef.pNext      = nullptr;
        resolvedInputRef.attachment = 1;
        resolvedInputRef.layout     = VK_IMAGE_LAYOUT_GENERAL;
        resolvedInputRef.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;

        VkAttachmentReference2 colorRef;
        colorRef.sType      = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
        colorRef.pNext      = nullptr;
        colorRef.attachment = 2;
        colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorRef.aspectMask = 0;

        VkSubpassDescriptionDepthStencilResolve depthResolve;
        depthResolve.sType            = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE;
        depthResolve.pNext            = nullptr;
        depthResolve.depthResolveMode = depthResolveMode;
        depthResolve.stencilResolveMode = VK_RESOLVE_MODE_NONE;
        depthResolve.pDepthStencilResolveAttachment = &depthResolveRef;

        VkSubpassDescription2 subpassDescriptions[2];
        // subpass 0: the hardware resolve, no draws.
        subpassDescriptions[0].sType                   = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
        subpassDescriptions[0].pNext                   = &depthResolve;
        subpassDescriptions[0].flags                   = 0;
        subpassDescriptions[0].pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpassDescriptions[0].viewMask                = 0;
        subpassDescriptions[0].inputAttachmentCount    = 0;
        subpassDescriptions[0].pInputAttachments       = nullptr;
        subpassDescriptions[0].colorAttachmentCount    = 0;
        subpassDescriptions[0].pColorAttachments       = nullptr;
        subpassDescriptions[0].pResolveAttachments     = nullptr;
        subpassDescriptions[0].pDepthStencilAttachment = &depthInputRef;
        subpassDescriptions[0].preserveAttachmentCount = 0;
        subpassDescriptions[0].pPreserveAttachments    = nullptr;

        // subpass 1: the fullscreen transform draw.
        subpassDescriptions[1].sType                   = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
        subpassDescriptions[1].pNext                   = nullptr;
        subpassDescriptions[1].flags                   = 0;
        subpassDescriptions[1].pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpassDescriptions[1].viewMask                = 0;
        subpassDescriptions[1].inputAttachmentCount    = 1;
        subpassDescriptions[1].pInputAttachments       = &resolvedInputRef;
        subpassDescriptions[1].colorAttachmentCount    = 1;
        subpassDescriptions[1].pColorAttachments       = &colorRef;
        subpassDescriptions[1].pResolveAttachments     = nullptr;
        subpassDescriptions[1].pDepthStencilAttachment = nullptr;
        subpassDescriptions[1].preserveAttachmentCount = 0;
        subpassDescriptions[1].pPreserveAttachments    = nullptr;

        VkSubpassDependency2 subpassDependencies[4];

        // Wait for the game's depth writes to complete before resolving.
        subpassDependencies[0].sType           = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
        subpassDependencies[0].pNext           = nullptr;
        subpassDependencies[0].srcSubpass      = VK_SUBPASS_EXTERNAL;
        subpassDependencies[0].dstSubpass      = 0;
        subpassDependencies[0].srcStageMask    = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        subpassDependencies[0].dstStageMask    = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        subpassDependencies[0].srcAccessMask   = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        subpassDependencies[0].dstAccessMask   = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        subpassDependencies[0].dependencyFlags = 0;
        subpassDependencies[0].viewOffset      = 0;

        // resolve write -> sampling read, and the intermediate's layout change.
        subpassDependencies[1].sType           = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
        subpassDependencies[1].pNext           = nullptr;
        subpassDependencies[1].srcSubpass      = 0;
        subpassDependencies[1].dstSubpass      = 1;
        subpassDependencies[1].srcStageMask    = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        subpassDependencies[1].dstStageMask    = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        subpassDependencies[1].srcAccessMask   = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        subpassDependencies[1].dstAccessMask   = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
        subpassDependencies[1].dependencyFlags = 0;
        subpassDependencies[1].viewOffset      = 0;

        // Incoming: wait for prior color writes before this render pass
        subpassDependencies[2].sType           = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
        subpassDependencies[2].pNext           = nullptr;
        subpassDependencies[2].srcSubpass      = VK_SUBPASS_EXTERNAL;
        subpassDependencies[2].dstSubpass      = 1;
        subpassDependencies[2].srcStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        subpassDependencies[2].dstStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        subpassDependencies[2].srcAccessMask   = 0;
        subpassDependencies[2].dstAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        subpassDependencies[2].dependencyFlags = 0;
        subpassDependencies[2].viewOffset      = 0;

        // Make the transformed depth visible to the effects' shader reads.
        subpassDependencies[3].sType           = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
        subpassDependencies[3].pNext           = nullptr;
        subpassDependencies[3].srcSubpass      = 1;
        subpassDependencies[3].dstSubpass      = VK_SUBPASS_EXTERNAL;
        subpassDependencies[3].srcStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        subpassDependencies[3].dstStageMask    = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        subpassDependencies[3].srcAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        subpassDependencies[3].dstAccessMask   = VK_ACCESS_SHADER_READ_BIT;
        subpassDependencies[3].dependencyFlags = 0;
        subpassDependencies[3].viewOffset      = 0;

        VkRenderPassCreateInfo2 renderPassCreateInfo;
        renderPassCreateInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2;
        renderPassCreateInfo.pNext           = nullptr;
        renderPassCreateInfo.flags           = 0;
        renderPassCreateInfo.attachmentCount = 3;
        renderPassCreateInfo.pAttachments    = attachments;
        renderPassCreateInfo.subpassCount    = 2;
        renderPassCreateInfo.pSubpasses      = subpassDescriptions;
        renderPassCreateInfo.dependencyCount = 4;
        renderPassCreateInfo.pDependencies   = subpassDependencies;
        renderPassCreateInfo.correlatedViewMaskCount = 0;
        renderPassCreateInfo.pCorrelatedViewMasks    = nullptr;

        VkRenderPass renderPass = VK_NULL_HANDLE;
        VkResult result = logicalDevice->vkd.CreateRenderPass2(logicalDevice->device, &renderPassCreateInfo, nullptr, &renderPass);
        ASSERT_VULKAN_VAL(result, VK_NULL_HANDLE);

        return renderPass;
    }
} // namespace VKIntox
