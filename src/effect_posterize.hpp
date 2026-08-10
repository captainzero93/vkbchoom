#ifndef EFFECT_POSTERIZE_HPP_INCLUDED
#define EFFECT_POSTERIZE_HPP_INCLUDED
#include <vector>
#include <fstream>
#include <string>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <memory>

#include "vulkan_include.hpp"

#include "effect.hpp"
#include "config.hpp"

#include "logical_device.hpp"

namespace vkbChoom
{
    // Colour-flatten/posterize pass. Same single-binding shape as
    // RcasEffect/BwrEffect -- pure colour-space work, no depth needed.
    class PosterizeEffect : public Effect
    {
    public:
        PosterizeEffect(LogicalDevice*       pLogicalDevice,
                        VkFormat             format,
                        VkExtent2D           imageExtent,
                        std::vector<VkImage> inputImages,
                        std::vector<VkImage> outputImages,
                        Config*              pConfig);
        void applyEffect(uint32_t imageIndex, VkCommandBuffer commandBuffer) override;
        ~PosterizeEffect();

    private:
        LogicalDevice*               pLogicalDevice;
        std::vector<VkImage>         inputImages;
        std::vector<VkImage>         outputImages;
        std::vector<VkImageView>     inputImageViews;
        std::vector<VkImageView>     outputImageViews;
        std::vector<VkDescriptorSet> imageDescriptorSets;
        std::vector<VkFramebuffer>   framebuffers;
        VkDescriptorSetLayout        imageSamplerDescriptorSetLayout;
        VkDescriptorPool             descriptorPool;
        VkShaderModule               vertexModule;
        VkShaderModule               fragmentModule;
        VkRenderPass                 renderPass;
        VkPipelineLayout             pipelineLayout;
        VkPipeline                   pipeline;
        VkSampler                    sampler;
        VkExtent2D                   imageExtent;
        VkFormat                     format;

        Config* pConfig;
    };
} // namespace vkbChoom

#endif // EFFECT_POSTERIZE_HPP_INCLUDED
