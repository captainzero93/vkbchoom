#ifndef SAMPLER_HPP_INCLUDED
#define SAMPLER_HPP_INCLUDED
#include <vector>
#include <fstream>
#include <string>
#include <iostream>
#include <vector>
#include <memory>

#include "vulkan_include.hpp"

#include "logical_device.hpp"

namespace vkbChoom
{
    VkSampler createSampler(LogicalDevice* pLogicalDevice);
} // namespace vkbChoom

#endif // SAMPLER_HPP_INCLUDED
