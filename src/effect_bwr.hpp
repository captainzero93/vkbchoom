#ifndef EFFECT_BWR_HPP_INCLUDED
#define EFFECT_BWR_HPP_INCLUDED
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
    // Black/white/red selective-colour pass. Same shape as RcasEffect --
    // single colour input, no depth needed -- since this is a pure
    // colour-space stylisation, not a geometry-aware one.
    class BwrEffect : public Effect
    {
    public:
        BwrEffect(LogicalDevice*       pLogicalDevice,
                  VkFormat             format,
                  VkExtent2D           imageExtent,
                  std::vector<VkImage> inputImages,
                  std::vector<VkImage> outputImages,
                  Config*              pConfig);
        void applyEffect(uint32_t imageIndex, VkCommandBuffer commandBuffer) override;
        ~BwrEffect();

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

#endif // EFFECT_BWR_HPP_INCLUDED
