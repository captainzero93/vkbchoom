#include "effect_posterize.hpp"

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
    PosterizeEffect::PosterizeEffect(LogicalDevice*       pLogicalDevice,
                                     VkFormat             format,
                                     VkExtent2D           imageExtent,
                                     std::vector<VkImage> inputImages,
                                     std::vector<VkImage> outputImages,
                                     Config*              pConfig)
    {
        Logger::debug("in creating PosterizeEffect");

        this->pLogicalDevice = pLogicalDevice;
        this->format         = format;
        this->imageExtent    = imageExtent;
        this->inputImages    = inputImages;
        this->outputImages   = outputImages;
        this->pConfig        = pConfig;

        inputImageViews  = createImageViews(pLogicalDevice, format, inputImages);
        outputImageViews = createImageViews(pLogicalDevice, format, outputImages);
        sampler          = createSampler(pLogicalDevice);

        imageSamplerDescriptorSetLayout = createImageSamplerDescriptorSetLayout(pLogicalDevice, 1);

        VkDescriptorPoolSize imagePoolSize;
        imagePoolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        imagePoolSize.descriptorCount = static_cast<uint32_t>(inputImages.size());

        std::vector<VkDescriptorPoolSize> poolSizes = {imagePoolSize};
        descriptorPool                              = createDescriptorPool(pLogicalDevice, poolSizes);

        createShaderModule(pLogicalDevice, full_screen_triangle_vert, &vertexModule);
        createShaderModule(pLogicalDevice, posterize_frag, &fragmentModule);

        renderPass = createRenderPass(pLogicalDevice, format);

        std::vector<VkDescriptorSetLayout> descriptorSetLayouts = {imageSamplerDescriptorSetLayout};
        pipelineLayout                                          = createGraphicsPipelineLayout(pLogicalDevice, descriptorSetLayouts);

        struct PosterizeOptions
        {
            float levels;
        };
        PosterizeOptions options;
        options.levels = pConfig->getOption<float>("posterizeLevels", 6.0f);

        VkSpecializationMapEntry specMapEntry;
        specMapEntry.constantID = 0;
        specMapEntry.offset     = 0;
        specMapEntry.size       = sizeof(float);

        VkSpecializationInfo specializationInfo;
        specializationInfo.mapEntryCount = 1;
        specializationInfo.pMapEntries   = &specMapEntry;
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

        std::vector<std::vector<VkImageView>> imageViewsVector = {inputImageViews};

        imageDescriptorSets = allocateAndWriteImageSamplerDescriptorSets(
            pLogicalDevice, descriptorPool, imageSamplerDescriptorSetLayout, std::vector<VkSampler>(1, sampler), imageViewsVector);

        framebuffers = createFramebuffers(pLogicalDevice, renderPass, imageExtent, {outputImageViews});
    }

    void PosterizeEffect::applyEffect(uint32_t imageIndex, VkCommandBuffer commandBuffer)
    {
        Logger::debug("applying posterize effect to cb " + convertToString(commandBuffer));

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

    PosterizeEffect::~PosterizeEffect()
    {
        Logger::debug("destroying posterize effect " + convertToString(this));
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

        pLogicalDevice->vkd.DestroySampler(pLogicalDevice->device, sampler, nullptr);
    }

} // namespace vkbChoom
