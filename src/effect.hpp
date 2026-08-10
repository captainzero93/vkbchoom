#ifndef EFFECT_HPP_INCLUDED
#define EFFECT_HPP_INCLUDED
#include <vector>
#include <fstream>
#include <string>
#include <iostream>
#include <vector>
#include <unordered_map>

#include "vulkan_include.hpp"

namespace vkbChoom
{
    class Effect
    {
    public:
        void virtual applyEffect(uint32_t imageIndex, VkCommandBuffer commandBuffer) = 0;
        void virtual updateEffect(){};
        void virtual useDepthImage(VkImageView depthImageView){};
        virtual ~Effect(){};

    private:
    };
} // namespace vkbChoom

#endif // EFFECT_HPP_INCLUDED
