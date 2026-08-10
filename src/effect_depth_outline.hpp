#ifndef EFFECT_DEPTH_OUTLINE_HPP_INCLUDED
#define EFFECT_DEPTH_OUTLINE_HPP_INCLUDED
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
    // Depth-based outline pass: two inputs (colour + depth) instead of one.
    //
    // useDepthImage() can be called with VK_NULL_HANDLE (no depth detected
    // yet) or with a real view, and can be called more than once over a
    // session as the depth-tracking plumbing in basalt.cpp rewires things.
    // Until a real depth view has actually arrived, applyEffect() falls
    // back to a plain copy (same shape as TransferEffect) rather than
    // running the outline shader against meaningless data -- there is no
    // guarantee the game's first depth-format image has been created and
    // bound by the time this effect is constructed.
    class DepthOutlineEffect : public Effect
    {
    public:
        DepthOutlineEffect(LogicalDevice*       pLogicalDevice,
                           VkFormat             format,
                           VkExtent2D           imageExtent,
                           std::vector<VkImage> inputImages,
                           std::vector<VkImage> outputImages,
                           Config*              pConfig);
        void applyEffect(uint32_t imageIndex, VkCommandBuffer commandBuffer) override;
        void useDepthImage(VkImageView depthImageView) override;
        ~DepthOutlineEffect();

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
        VkSampler                    colorSampler;
        VkSampler                    depthSampler;
        VkExtent2D                   imageExtent;
        VkFormat                     format;

        VkImageView currentDepthImageView = VK_NULL_HANDLE;
        bool        hasRealDepth          = false;

        Config* pConfig;
    };
} // namespace vkbChoom

#endif // EFFECT_DEPTH_OUTLINE_HPP_INCLUDED
