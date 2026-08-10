#ifndef LOGICAL_SWAPCHAIN_HPP_INCLUDED
#define LOGICAL_SWAPCHAIN_HPP_INCLUDED
#include <vector>
#include <fstream>
#include <string>
#include <iostream>
#include <vector>
#include <memory>

#include "effect.hpp"

#include "vulkan_include.hpp"

#include "logical_device.hpp"

namespace vkbChoom
{
    // for each swapchain, we have the Images and the other stuff we need to execute the compute shader
    struct LogicalSwapchain
    {
        LogicalDevice*                       pLogicalDevice;
        VkSwapchainCreateInfoKHR             swapchainCreateInfo;
        VkExtent2D                           imageExtent;
        VkFormat                             format;
        uint32_t                             imageCount;
        std::vector<VkImage>                 images;
        std::vector<VkImage>                 fakeImages;
        std::vector<VkCommandBuffer>         commandBuffersEffect;
        std::vector<VkCommandBuffer>         commandBuffersNoEffect;
        std::vector<VkSemaphore>             semaphores;
        std::vector<std::shared_ptr<Effect>> effects;
        std::shared_ptr<Effect>              defaultTransfer;
        VkDeviceMemory                       fakeImageMemory;

        // Which create mode actually succeeded for THIS swapchain. The device
        // may support mutable format on paper while the driver still rejects
        // the modified create at runtime, so this can't live on LogicalDevice.
        bool useMutableFormat = false; // render effects directly into swapchain image views
        bool bypass           = false; // driver rejected all modified creates: pure passthrough, no effects

        // One-shot "effect is active" confirmation, per swapchain rather than
        // per process. A single process can go through several of these --
        // MGSV/DXVK renegotiate display mode more than once on the way from
        // launch to actual gameplay, each time tearing down and recreating
        // the swapchain -- and a log guarded by a function-local static only
        // ever confirms the *first* one. See "Notes for maintainers".
        bool submitLogged = false;

        void destroy();
    };
} // namespace vkbChoom

#endif // LOGICAL_SWAPCHAIN_HPP_INCLUDED
