#include "effect_depth_outline.hpp"

#include "image_view.hpp"
#include "descriptor_set.hpp"
#include "renderpass.hpp"
#include "graphics_pipeline.hpp"
#include "framebuffer.hpp"
#include "shader.hpp"
#include "sampler.hpp"
#include "util.hpp"

#include "shader_sources.hpp"

namespace vkbChoom
{
    // Vulkan only guarantees VK_FILTER_NEAREST support for sampling depth
    // formats -- linear filtering of a depth image is an optional feature
    // implementations may not expose. createSampler() (used for the colour
    // input) is hardcoded to linear, so the depth binding gets its own
    // nearest-filter sampler here rather than risking an unsupported
    // combination on hardware/drivers that don't offer linear depth sampling.
    static VkSampler createNearestSampler(LogicalDevice* pLogicalDevice)
    {
        VkSampler sampler;

        VkSamplerCreateInfo samplerCreateInfo;
        samplerCreateInfo.sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerCreateInfo.pNext                   = nullptr;
        samplerCreateInfo.flags                   = 0;
        samplerCreateInfo.magFilter               = VK_FILTER_NEAREST;
        samplerCreateInfo.minFilter               = VK_FILTER_NEAREST;
        samplerCreateInfo.mipmapMode              = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerCreateInfo.addressModeU            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerCreateInfo.addressModeV            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerCreateInfo.addressModeW            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerCreateInfo.mipLodBias              = 0.0f;
        samplerCreateInfo.anisotropyEnable        = VK_FALSE;
        samplerCreateInfo.maxAnisotropy           = 1;
        samplerCreateInfo.compareEnable           = VK_FALSE;
        samplerCreateInfo.compareOp               = VK_COMPARE_OP_ALWAYS;
        samplerCreateInfo.minLod                  = 0.0f;
        samplerCreateInfo.maxLod                  = 0.0f;
        samplerCreateInfo.borderColor             = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        samplerCreateInfo.unnormalizedCoordinates = VK_FALSE;

        VkResult result = pLogicalDevice->vkd.CreateSampler(pLogicalDevice->device, &samplerCreateInfo, nullptr, &sampler);
        ASSERT_VULKAN(result);
        return sampler;
    }

    DepthOutlineEffect::DepthOutlineEffect(LogicalDevice*       pLogicalDevice,
                                           VkFormat             format,
                                           VkExtent2D           imageExtent,
                                           std::vector<VkImage> inputImages,
                                           std::vector<VkImage> outputImages,
                                           Config*              pConfig)
    {
        Logger::debug("in creating DepthOutlineEffect");

        this->pLogicalDevice = pLogicalDevice;
        this->format         = format;
        this->imageExtent    = imageExtent;
        this->inputImages    = inputImages;
        this->outputImages   = outputImages;
        this->pConfig        = pConfig;

        inputImageViews  = createImageViews(pLogicalDevice, format, inputImages);
        outputImageViews = createImageViews(pLogicalDevice, format, outputImages);

        colorSampler = createSampler(pLogicalDevice);
        depthSampler = createNearestSampler(pLogicalDevice);

        // Two bindings: colour (0) and depth (1).
        imageSamplerDescriptorSetLayout = createImageSamplerDescriptorSetLayout(pLogicalDevice, 2);

        VkDescriptorPoolSize imagePoolSize;
        imagePoolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        imagePoolSize.descriptorCount = static_cast<uint32_t>(inputImages.size()) * 2;

        std::vector<VkDescriptorPoolSize> poolSizes = {imagePoolSize};
        descriptorPool                              = createDescriptorPool(pLogicalDevice, poolSizes);

        createShaderModule(pLogicalDevice, full_screen_triangle_vert, &vertexModule);
        createShaderModule(pLogicalDevice, depth_outline_frag, &fragmentModule);

        renderPass = createRenderPass(pLogicalDevice, format);

        std::vector<VkDescriptorSetLayout> descriptorSetLayouts = {imageSamplerDescriptorSetLayout};
        pipelineLayout                                          = createGraphicsPipelineLayout(pLogicalDevice, descriptorSetLayouts);

        struct DepthOutlineOptions
        {
            float threshold;
            float debugShowDepth;
        };
        DepthOutlineOptions options;
        options.threshold      = pConfig->getOption<float>("depthOutlineThreshold", 0.003f);
        options.debugShowDepth = pConfig->getOption<std::string>("depthOutlineDebugView", "off") == "on" ? 1.0f : 0.0f;

        std::vector<VkSpecializationMapEntry> specMapEntries(2);
        specMapEntries[0].constantID = 0;
        specMapEntries[0].offset     = offsetof(DepthOutlineOptions, threshold);
        specMapEntries[0].size       = sizeof(float);
        specMapEntries[1].constantID = 1;
        specMapEntries[1].offset     = offsetof(DepthOutlineOptions, debugShowDepth);
        specMapEntries[1].size       = sizeof(float);

        VkSpecializationInfo specializationInfo;
        specializationInfo.mapEntryCount = specMapEntries.size();
        specializationInfo.pMapEntries   = specMapEntries.data();
        specializationInfo.dataSize      = sizeof(options);
        specializationInfo.pData         = &options;

        pipeline = createGraphicsPipeline(pLogicalDevice,
                                          vertexModule,
                                          nullptr,
                                          "main",
                                          fragmentModule,
                                          &specializationInfo,
                                          "main",
                                          imageExtent,
                                          renderPass,
                                          pipelineLayout);

        // Binding 1 (depth) has no real depth image yet at construction time
        // -- there's no guarantee the game's first depth-format resource has
        // been created and bound before this effect exists. Point it at the
        // colour views as a harmless placeholder; applyEffect() will not
        // actually sample it until useDepthImage() supplies a real view and
        // hasRealDepth flips true.
        std::vector<std::vector<VkImageView>> imageViewsVector = {inputImageViews, inputImageViews};

        imageDescriptorSets = allocateAndWriteImageSamplerDescriptorSets(
            pLogicalDevice, descriptorPool, imageSamplerDescriptorSetLayout, std::vector<VkSampler>{colorSampler, depthSampler}, imageViewsVector);

        framebuffers = createFramebuffers(pLogicalDevice, renderPass, imageExtent, {outputImageViews});
    }

    void DepthOutlineEffect::useDepthImage(VkImageView depthImageView)
    {
        if (depthImageView == VK_NULL_HANDLE || depthImageView == currentDepthImageView)
            return;

        currentDepthImageView = depthImageView;
        hasRealDepth          = true;

        VkDescriptorImageInfo depthInfo;
        depthInfo.sampler     = depthSampler;
        depthInfo.imageView   = depthImageView;
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write = {};
        write.sType                = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.pNext                = nullptr;
        write.dstBinding           = 1;
        write.dstArrayElement      = 0;
        write.descriptorCount      = 1;
        write.descriptorType       = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo           = &depthInfo;
        write.pBufferInfo          = nullptr;
        write.pTexelBufferView     = nullptr;

        for (auto& ds : imageDescriptorSets)
        {
            write.dstSet = ds;
            pLogicalDevice->vkd.UpdateDescriptorSets(pLogicalDevice->device, 1, &write, 0, nullptr);
        }

        Logger::info("DepthOutlineEffect: real depth image bound, outline pass now active");
    }

    void DepthOutlineEffect::applyEffect(uint32_t imageIndex, VkCommandBuffer commandBuffer)
    {
        if (!hasRealDepth)
        {
            // No real depth yet -- pass the frame through unchanged, same
            // shape as TransferEffect's copy, so the chain isn't broken
            // while waiting for a depth image to show up.
            Logger::debug("DepthOutlineEffect: no depth yet, passing through");

            VkImageCopy imageCopy;
            imageCopy.srcSubresource            = {};
            imageCopy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            imageCopy.srcSubresource.layerCount = 1;
            imageCopy.srcOffset                 = {};
            imageCopy.dstSubresource            = {};
            imageCopy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            imageCopy.dstSubresource.layerCount = 1;
            imageCopy.dstOffset                 = {};
            imageCopy.extent                    = {imageExtent.width, imageExtent.height, 1};

            VkImageMemoryBarrier memoryBarrier;
            memoryBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            memoryBarrier.pNext               = nullptr;
            memoryBarrier.srcAccessMask       = 0;
            memoryBarrier.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
            memoryBarrier.oldLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            memoryBarrier.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            memoryBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            memoryBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            memoryBarrier.image               = inputImages[imageIndex];

            memoryBarrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
            memoryBarrier.subresourceRange.baseMipLevel   = 0;
            memoryBarrier.subresourceRange.levelCount     = 1;
            memoryBarrier.subresourceRange.baseArrayLayer = 0;
            memoryBarrier.subresourceRange.layerCount     = 1;

            pLogicalDevice->vkd.CmdPipelineBarrier(
                commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &memoryBarrier);

            memoryBarrier.image         = outputImages[imageIndex];
            memoryBarrier.oldLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            memoryBarrier.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            memoryBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            pLogicalDevice->vkd.CmdPipelineBarrier(
                commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &memoryBarrier);

            pLogicalDevice->vkd.CmdCopyImage(commandBuffer,
                                             inputImages[imageIndex],
                                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                             outputImages[imageIndex],
                                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                             1,
                                             &imageCopy);

            memoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            memoryBarrier.dstAccessMask = 0;
            memoryBarrier.image         = outputImages[imageIndex];
            memoryBarrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            memoryBarrier.newLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            pLogicalDevice->vkd.CmdPipelineBarrier(
                commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &memoryBarrier);

            memoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            memoryBarrier.dstAccessMask = 0;
            memoryBarrier.image         = inputImages[imageIndex];
            memoryBarrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            memoryBarrier.newLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            pLogicalDevice->vkd.CmdPipelineBarrier(
                commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &memoryBarrier);

            return;
        }

        Logger::debug("applying depth outline effect to cb " + convertToString(commandBuffer));

        VkImageMemoryBarrier memoryBarrier;
        memoryBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        memoryBarrier.pNext               = nullptr;
        memoryBarrier.srcAccessMask       = VK_ACCESS_MEMORY_WRITE_BIT;
        memoryBarrier.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        memoryBarrier.oldLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        memoryBarrier.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        memoryBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        memoryBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        memoryBarrier.image               = inputImages[imageIndex];

        memoryBarrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        memoryBarrier.subresourceRange.baseMipLevel   = 0;
        memoryBarrier.subresourceRange.levelCount     = 1;
        memoryBarrier.subresourceRange.baseArrayLayer = 0;
        memoryBarrier.subresourceRange.layerCount     = 1;

        VkImageMemoryBarrier secondBarrier = memoryBarrier;
        secondBarrier.srcAccessMask        = VK_ACCESS_SHADER_READ_BIT;
        secondBarrier.dstAccessMask        = 0;
        secondBarrier.oldLayout            = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        secondBarrier.newLayout            = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        pLogicalDevice->vkd.CmdPipelineBarrier(
            commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &memoryBarrier);

        VkRenderPassBeginInfo renderPassBeginInfo;
        renderPassBeginInfo.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassBeginInfo.pNext             = nullptr;
        renderPassBeginInfo.renderPass        = renderPass;
        renderPassBeginInfo.framebuffer       = framebuffers[imageIndex];
        renderPassBeginInfo.renderArea.offset = {0, 0};
        renderPassBeginInfo.renderArea.extent = imageExtent;
        VkClearValue clearValue               = {0.0f, 0.0f, 0.0f, 1.0f};
        renderPassBeginInfo.clearValueCount   = 1;
        renderPassBeginInfo.pClearValues      = &clearValue;

        pLogicalDevice->vkd.CmdBeginRenderPass(commandBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

        pLogicalDevice->vkd.CmdBindDescriptorSets(
            commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &(imageDescriptorSets[imageIndex]), 0, nullptr);

        pLogicalDevice->vkd.CmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

        pLogicalDevice->vkd.CmdDraw(commandBuffer, 3, 1, 0, 0);

        pLogicalDevice->vkd.CmdEndRenderPass(commandBuffer);

        pLogicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                               0,
                                               0,
                                               nullptr,
                                               0,
                                               nullptr,
                                               1,
                                               &secondBarrier);
    }

    DepthOutlineEffect::~DepthOutlineEffect()
    {
        Logger::debug("destroying depth outline effect " + convertToString(this));
        pLogicalDevice->vkd.DestroyPipeline(pLogicalDevice->device, pipeline, nullptr);
        pLogicalDevice->vkd.DestroyPipelineLayout(pLogicalDevice->device, pipelineLayout, nullptr);
        pLogicalDevice->vkd.DestroyRenderPass(pLogicalDevice->device, renderPass, nullptr);
        pLogicalDevice->vkd.DestroyDescriptorSetLayout(pLogicalDevice->device, imageSamplerDescriptorSetLayout, nullptr);

        pLogicalDevice->vkd.DestroyShaderModule(pLogicalDevice->device, vertexModule, nullptr);
        pLogicalDevice->vkd.DestroyShaderModule(pLogicalDevice->device, fragmentModule, nullptr);

        pLogicalDevice->vkd.DestroyDescriptorPool(pLogicalDevice->device, descriptorPool, nullptr);

        for (unsigned int i = 0; i < framebuffers.size(); i++)
        {
            pLogicalDevice->vkd.DestroyFramebuffer(pLogicalDevice->device, framebuffers[i], nullptr);
            pLogicalDevice->vkd.DestroyImageView(pLogicalDevice->device, inputImageViews[i], nullptr);
            pLogicalDevice->vkd.DestroyImageView(pLogicalDevice->device, outputImageViews[i], nullptr);
        }

        pLogicalDevice->vkd.DestroySampler(pLogicalDevice->device, colorSampler, nullptr);
        pLogicalDevice->vkd.DestroySampler(pLogicalDevice->device, depthSampler, nullptr);
    }

} // namespace vkbChoom
