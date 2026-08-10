#include "vulkan_include.hpp"

#include "vulkan/vk_layer.h"

#define NOMINMAX
#include <windows.h>

#include <mutex>
#include <map>
#include <vector>
#include <unordered_map>
#include <iostream>
#include <string>
#include <memory>
#include <cstring>
#include <cctype>

#include "util.hpp"

#include "logical_device.hpp"
#include "logical_swapchain.hpp"

#include "image_view.hpp"
#include "sampler.hpp"
#include "framebuffer.hpp"
#include "descriptor_set.hpp"
#include "shader.hpp"
#include "graphics_pipeline.hpp"
#include "command_buffer.hpp"
#include "buffer.hpp"
#include "config.hpp"
#include "fake_swapchain.hpp"
#include "renderpass.hpp"
#include "format.hpp"
#include "logger.hpp"
#include "platform_win32.hpp"

#include "effect.hpp"
#include "effect_bwr.hpp"
#include "effect_depth_outline.hpp"
#include "effect_posterize.hpp"
#include "effect_rcas.hpp"
#include "effect_smaa.hpp"
#include "effect_transfer.hpp"
#include "screenshot.hpp"

#define VKBCHOOM_NAME "VK_LAYER_VKBCHOOM_post_processing"

#if defined(_WIN32)
#define VKBCHOOM_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) && __GNUC__ >= 4
#define VKBCHOOM_EXPORT __attribute__((visibility("default")))
#else
#error "Unsupported platform!"
#endif

namespace vkbChoom
{
    std::shared_ptr<Config> pConfig = nullptr;

    Logger Logger::s_instance;

    // layer book-keeping information, to store dispatch tables by key
    std::unordered_map<void*, InstanceDispatch>                           instanceDispatchMap;
    std::unordered_map<void*, VkInstance>                                 instanceMap;
    std::unordered_map<void*, uint32_t>                                   instanceVersionMap;
    std::unordered_map<void*, std::shared_ptr<LogicalDevice>>             deviceMap;
    std::unordered_map<VkSwapchainKHR, std::shared_ptr<LogicalSwapchain>> swapchainMap;

    std::mutex globalLock;
#ifdef _GCC_
    using scoped_lock __attribute__((unused)) = std::lock_guard<std::mutex>;
#else
    using scoped_lock = std::lock_guard<std::mutex>;
#endif

    namespace
    {
        bool equalsIgnoreCaseAscii(const std::string& a, const std::string& b)
        {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); i++)
            {
                if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                    return false;
            }
            return true;
        }
    } // namespace

    // Whether this process is the one the layer should actually do anything
    // in. vkbchoom.json registers as a Vulkan GLOBAL implicit layer -- the
    // loader has no "only for this one game" concept, so any process that
    // creates a Vulkan instance gets smaa_layer.dll loaded and asked to
    // participate, which is how this ended up quietly running inside
    // chaiNNer's bundled python.exe too. Every real interception in this
    // file funnels through INTERCEPT_CALLS, which checks this first; outside
    // the target process it's a silent passthrough. See "Scope" in the
    // README and the matching note under "Notes for maintainers".
    bool isTargetProcess()
    {
        static const bool result = equalsIgnoreCaseAscii(exeName(), pConfig->getOption<std::string>("targetExecutable", "mgsvtpp.exe"));
        return result;
    }

    template<typename DispatchableType>
    void* GetKey(DispatchableType inst)
    {
        return *(void**) inst;
    }

    VkResult VKAPI_CALL vkbChoom_CreateInstance(const VkInstanceCreateInfo*  pCreateInfo,
                                                const VkAllocationCallbacks* pAllocator,
                                                VkInstance*                  pInstance)
    {
        VkLayerInstanceCreateInfo* layerCreateInfo = (VkLayerInstanceCreateInfo*) pCreateInfo->pNext;

        // step through the chain of pNext until we get to the link info
        while (layerCreateInfo
               && (layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO || layerCreateInfo->function != VK_LAYER_LINK_INFO))
        {
            layerCreateInfo = (VkLayerInstanceCreateInfo*) layerCreateInfo->pNext;
        }

        Logger::trace("vkCreateInstance");

        if (layerCreateInfo == nullptr)
        {
            // No loader instance create info
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        PFN_vkGetInstanceProcAddr gpa = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
        // move chain on for next layer
        layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

        PFN_vkCreateInstance createFunc = (PFN_vkCreateInstance) gpa(VK_NULL_HANDLE, "vkCreateInstance");

        VkInstanceCreateInfo modifiedCreateInfo = *pCreateInfo;
        VkApplicationInfo    appInfo;
        if (modifiedCreateInfo.pApplicationInfo)
        {
            appInfo = *(modifiedCreateInfo.pApplicationInfo);
            if (appInfo.apiVersion < VK_API_VERSION_1_1)
            {
                appInfo.apiVersion = VK_API_VERSION_1_1;
            }
        }
        else
        {
            appInfo.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
            appInfo.pNext              = nullptr;
            appInfo.pApplicationName   = nullptr;
            appInfo.applicationVersion = 0;
            appInfo.pEngineName        = nullptr;
            appInfo.engineVersion      = 0;
            appInfo.apiVersion         = VK_API_VERSION_1_1;
        }

        modifiedCreateInfo.pApplicationInfo = &appInfo;
        VkResult ret                        = createFunc(&modifiedCreateInfo, pAllocator, pInstance);

        // If the next layer down failed, *pInstance is undefined. The old
        // code went straight on to fillDispatchTableInstance(*pInstance, ...)
        // and GetKey(), which does *(void**)inst -- dereferencing garbage.
        // That is an access violation with no log entry, i.e. exactly the
        // "game just doesn't start / nothing in the log" signature.
        if (ret != VK_SUCCESS)
        {
            Logger::err("next-layer vkCreateInstance failed with VkResult " + std::to_string(ret)
                        + " -- passing the failure up untouched");
            return ret;
        }

        // fetch our own dispatch table for the functions we need, into the next layer
        InstanceDispatch dispatchTable;
        fillDispatchTableInstance(*pInstance, gpa, &dispatchTable);

        // store the table by key
        {
            scoped_lock l(globalLock);
            instanceDispatchMap[GetKey(*pInstance)] = dispatchTable;
            instanceMap[GetKey(*pInstance)]         = *pInstance;
            instanceVersionMap[GetKey(*pInstance)]  = modifiedCreateInfo.pApplicationInfo->apiVersion;
        }

        return ret;
    }

    void VKAPI_CALL vkbChoom_DestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator)
    {
        if (!instance)
            return;

        scoped_lock l(globalLock);

        Logger::trace("vkDestroyInstance");

        InstanceDispatch dispatchTable = instanceDispatchMap[GetKey(instance)];

        dispatchTable.DestroyInstance(instance, pAllocator);

        instanceDispatchMap.erase(GetKey(instance));
        instanceMap.erase(GetKey(instance));
        instanceVersionMap.erase(GetKey(instance));
    }

    VkResult VKAPI_CALL vkbChoom_CreateDevice(VkPhysicalDevice             physicalDevice,
                                              const VkDeviceCreateInfo*    pCreateInfo,
                                              const VkAllocationCallbacks* pAllocator,
                                              VkDevice*                    pDevice)
    {
        scoped_lock l(globalLock);
        Logger::trace("vkCreateDevice");
        VkLayerDeviceCreateInfo* layerCreateInfo = (VkLayerDeviceCreateInfo*) pCreateInfo->pNext;

        // step through the chain of pNext until we get to the link info
        while (layerCreateInfo
               && (layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO || layerCreateInfo->function != VK_LAYER_LINK_INFO))
        {
            layerCreateInfo = (VkLayerDeviceCreateInfo*) layerCreateInfo->pNext;
        }

        if (layerCreateInfo == nullptr)
        {
            // No loader instance create info
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        PFN_vkGetInstanceProcAddr gipa = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
        PFN_vkGetDeviceProcAddr   gdpa = layerCreateInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;
        // move chain on for next layer
        layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

        PFN_vkCreateDevice createFunc = (PFN_vkCreateDevice) gipa(VK_NULL_HANDLE, "vkCreateDevice");

        // check and activate extentions
        uint32_t extensionCount = 0;

        instanceDispatchMap[GetKey(physicalDevice)].EnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> extensionProperties(extensionCount);
        instanceDispatchMap[GetKey(physicalDevice)].EnumerateDeviceExtensionProperties(
            physicalDevice, nullptr, &extensionCount, extensionProperties.data());

        bool supportsMutableFormat = false;
        for (VkExtensionProperties properties : extensionProperties)
        {
            if (properties.extensionName == std::string("VK_KHR_swapchain_mutable_format"))
            {
                Logger::debug("device supports VK_KHR_swapchain_mutable_format");
                supportsMutableFormat = true;
                break;
            }
        }

        VkPhysicalDeviceProperties deviceProps;
        instanceDispatchMap[GetKey(physicalDevice)].GetPhysicalDeviceProperties(physicalDevice, &deviceProps);

        VkDeviceCreateInfo       modifiedCreateInfo = *pCreateInfo;
        std::vector<const char*> enabledExtensionNames;

        // Only touch the extension list if the config actually asks for the
        // mutable-format render path. In transfer/bypass mode the device
        // creation stays byte-identical to what the app sent -- the swapchain
        // path doesn't need the extension, and every unnecessary difference
        // from an unlayered launch is one more thing that can upset whatever
        // in the chain is returning -3.
        std::string renderMode  = pConfig ? pConfig->getOption<std::string>("renderMode", "transfer") : "transfer";
        bool        wantMutable = (renderMode == "mutable");
        Logger::info("CreateDevice: renderMode = " + renderMode + ", mutable_format supported = " + std::to_string(supportsMutableFormat));

        if (wantMutable && supportsMutableFormat)
        {
            if (modifiedCreateInfo.enabledExtensionCount)
            {
                enabledExtensionNames = std::vector<const char*>(
                    modifiedCreateInfo.ppEnabledExtensionNames,
                    modifiedCreateInfo.ppEnabledExtensionNames + modifiedCreateInfo.enabledExtensionCount);
            }
            Logger::debug("activating mutable_format");
            addUniqueCString(enabledExtensionNames, "VK_KHR_swapchain_mutable_format");
            if (deviceProps.apiVersion < VK_API_VERSION_1_2 || instanceVersionMap[GetKey(physicalDevice)] < VK_API_VERSION_1_2)
            {
                addUniqueCString(enabledExtensionNames, "VK_KHR_image_format_list");
            }
            modifiedCreateInfo.ppEnabledExtensionNames = enabledExtensionNames.data();
            modifiedCreateInfo.enabledExtensionCount   = enabledExtensionNames.size();
        }

        // Active needed Features. Careful: if the app chains a
        // VkPhysicalDeviceFeatures2 in pNext (DXVK always does), the spec
        // requires pEnabledFeatures to be NULL
        // (VUID-VkDeviceCreateInfo-pNext-00373) -- setting both is invalid
        // usage that drivers are free to punish with obscure failures.
        // DXVK enables shaderImageGatherExtended itself (D3D11 gather4
        // requires it), so in that case we leave features entirely alone.
        bool appChainsFeatures2 = false;
        for (const VkBaseInStructure* s = (const VkBaseInStructure*) pCreateInfo->pNext; s; s = s->pNext)
        {
            if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)
            {
                appChainsFeatures2 = true;
                break;
            }
        }

        VkPhysicalDeviceFeatures deviceFeatures = {};
        if (appChainsFeatures2)
        {
            Logger::info("app chains VkPhysicalDeviceFeatures2 -- leaving pEnabledFeatures untouched");
        }
        else
        {
            if (modifiedCreateInfo.pEnabledFeatures)
            {
                deviceFeatures = *(modifiedCreateInfo.pEnabledFeatures);
            }
            deviceFeatures.shaderImageGatherExtended = VK_TRUE;
            modifiedCreateInfo.pEnabledFeatures      = &deviceFeatures;
        }

        VkResult ret = createFunc(physicalDevice, &modifiedCreateInfo, pAllocator, pDevice);

        if (ret != VK_SUCCESS)
            return ret;

        std::shared_ptr<LogicalDevice> pLogicalDevice(new LogicalDevice());
        pLogicalDevice->vki                   = instanceDispatchMap[GetKey(physicalDevice)];
        pLogicalDevice->device                = *pDevice;
        pLogicalDevice->physicalDevice        = physicalDevice;
        pLogicalDevice->instance              = instanceMap[GetKey(physicalDevice)];
        pLogicalDevice->queue                 = VK_NULL_HANDLE;
        pLogicalDevice->queueFamilyIndex      = 0;
        pLogicalDevice->commandPool           = VK_NULL_HANDLE;
        // effective capability: supported AND actually enabled on this device
        pLogicalDevice->supportsMutableFormat = supportsMutableFormat && wantMutable;

        fillDispatchTableDevice(*pDevice, gdpa, &pLogicalDevice->vkd);

        uint32_t count;

        pLogicalDevice->vki.GetPhysicalDeviceQueueFamilyProperties(pLogicalDevice->physicalDevice, &count, nullptr);

        std::vector<VkQueueFamilyProperties> queueProperties(count);

        pLogicalDevice->vki.GetPhysicalDeviceQueueFamilyProperties(pLogicalDevice->physicalDevice, &count, queueProperties.data());
        for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; i++)
        {
            auto& queueInfo = pCreateInfo->pQueueCreateInfos[i];
            if ((queueProperties[queueInfo.queueFamilyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT))
            {
                pLogicalDevice->vkd.GetDeviceQueue(pLogicalDevice->device, queueInfo.queueFamilyIndex, 0, &pLogicalDevice->queue);

                VkCommandPoolCreateInfo commandPoolCreateInfo;
                commandPoolCreateInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
                commandPoolCreateInfo.pNext            = nullptr;
                commandPoolCreateInfo.flags            = 0;
                commandPoolCreateInfo.queueFamilyIndex = queueInfo.queueFamilyIndex;

                Logger::debug("Found graphics capable queue");
                pLogicalDevice->vkd.CreateCommandPool(pLogicalDevice->device, &commandPoolCreateInfo, nullptr, &pLogicalDevice->commandPool);
                pLogicalDevice->queueFamilyIndex = queueInfo.queueFamilyIndex;

                initializeDispatchTable(pLogicalDevice->queue, pLogicalDevice->device);

                break;
            }
        }

        if (!pLogicalDevice->queue)
            Logger::err("Did not find a graphics queue!");

        deviceMap[GetKey(*pDevice)] = pLogicalDevice;

        return VK_SUCCESS;
    }

    void VKAPI_CALL vkbChoom_DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator)
    {
        if (!device)
            return;

        scoped_lock l(globalLock);

        Logger::trace("vkDestroyDevice");

        LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();
        if (pLogicalDevice->commandPool != VK_NULL_HANDLE)
        {
            Logger::debug("DestroyCommandPool");
            pLogicalDevice->vkd.DestroyCommandPool(device, pLogicalDevice->commandPool, pAllocator);
        }

        pLogicalDevice->vkd.DestroyDevice(device, pAllocator);

        deviceMap.erase(GetKey(device));
    }

    // Depth-format images the game creates get tracked in creation order
    // (depthImages/depthFormats/depthExtents), but only get an actual
    // VkImageView -- and become usable -- once BindImageMemory sees them
    // (depthImageViews, same indices, only as far as binding has reached).
    // Rather than blindly trusting whichever depth image happened to be
    // created first (the original vkBasalt-inherited behaviour, which in
    // practice locks onto whatever early setup/shadow-pass resource the
    // engine creates before the real per-frame scene depth buffer even
    // exists), this searches bound images for one whose resolution matches
    // a given target -- most recently bound first, so if more than one
    // candidate matches, the latest one wins.
    struct DepthMatch
    {
        VkImage     image  = VK_NULL_HANDLE;
        VkImageView view   = VK_NULL_HANDLE;
        VkFormat    format = VK_FORMAT_UNDEFINED;
    };

    static DepthMatch findMatchingDepthImage(LogicalDevice* pLogicalDevice, VkExtent2D targetExtent)
    {
        for (int i = static_cast<int>(pLogicalDevice->depthImageViews.size()) - 1; i >= 0; i--)
        {
            if (pLogicalDevice->depthExtents[i].width == targetExtent.width && pLogicalDevice->depthExtents[i].height == targetExtent.height)
            {
                return {pLogicalDevice->depthImages[i], pLogicalDevice->depthImageViews[i], pLogicalDevice->depthFormats[i]};
            }
        }
        return {};
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkbChoom_CreateSwapchainKHR(VkDevice                        device,
                                                               const VkSwapchainCreateInfoKHR* pCreateInfo,
                                                               const VkAllocationCallbacks*    pAllocator,
                                                               VkSwapchainKHR*                 pSwapchain)
    {
        scoped_lock l(globalLock);

        Logger::trace("vkCreateSwapchainKHR");

        LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();

        VkSwapchainCreateInfoKHR modifiedCreateInfo = *pCreateInfo;

        VkFormat format = modifiedCreateInfo.imageFormat;

        VkFormat srgbFormat  = isSRGB(format) ? format : convertToSRGB(format);
        VkFormat unormFormat = isSRGB(format) ? convertToUNORM(format) : format;
        Logger::debug(std::to_string(srgbFormat) + " " + std::to_string(unormFormat));

        {
            std::stringstream ss;
            ss << "CreateSwapchainKHR in: format=" << modifiedCreateInfo.imageFormat << " usage=0x" << std::hex << modifiedCreateInfo.imageUsage
               << " flags=0x" << modifiedCreateInfo.flags << std::dec << " extent=" << modifiedCreateInfo.imageExtent.width << "x"
               << modifiedCreateInfo.imageExtent.height;
            Logger::info(ss.str());
        }

        {
            std::string chain;
            for (const VkBaseInStructure* s = (const VkBaseInStructure*) pCreateInfo->pNext; s; s = s->pNext)
                chain += std::to_string((int) s->sType) + " ";
            Logger::info("CreateSwapchainKHR pNext sTypes: " + (chain.empty() ? std::string("(none)") : chain));
        }

        // DXVK 2.x uses VK_KHR_swapchain_mutable_format itself for sRGB views,
        // in which case the incoming pNext chain already contains a
        // VkImageFormatListCreateInfo. Prepending a second struct of the same
        // sType is invalid usage (VUID-VkSwapchainCreateInfoKHR-pNext) and can
        // make the driver fail or ignore one of the two lists. Detect it, merge
        // our required view formats into a combined list, and chain-skip the
        // app's struct instead of duplicating it.
        const VkImageFormatListCreateInfoKHR* appFormatList = nullptr;
        for (const VkBaseInStructure* s = (const VkBaseInStructure*) pCreateInfo->pNext; s; s = s->pNext)
        {
            if (s->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_KHR)
            {
                appFormatList = (const VkImageFormatListCreateInfoKHR*) s;
                Logger::info("app already chains VkImageFormatListCreateInfo with " + std::to_string(appFormatList->viewFormatCount)
                             + " formats -- merging instead of duplicating");
                break;
            }
        }

        std::vector<VkFormat> mergedFormats = {unormFormat};
        if (srgbFormat != unormFormat)
            mergedFormats.push_back(srgbFormat);
        if (appFormatList)
        {
            for (uint32_t i = 0; i < appFormatList->viewFormatCount; i++)
            {
                VkFormat f = appFormatList->pViewFormats[i];
                bool     present = false;
                for (VkFormat m : mergedFormats)
                    present |= (m == f);
                if (!present)
                    mergedFormats.push_back(f);
            }
        }

        VkImageFormatListCreateInfoKHR imageFormatListCreateInfo;
        if (pLogicalDevice->supportsMutableFormat)
        {
            // NOTE: |= rather than =. The original vkBasalt code replaced the
            // app's requested usage outright, which strips usage bits DXVK
            // relies on for its swapchain blitter/screenshot paths
            // (TRANSFER_SRC, sometimes STORAGE). Keep whatever DXVK asked for
            // and add what we need on top.
            modifiedCreateInfo.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                             | VK_IMAGE_USAGE_SAMPLED_BIT; // we want to use the swapchain images as output of the graphics pipeline
            modifiedCreateInfo.flags |= VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;

            imageFormatListCreateInfo.sType           = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_KHR;
            imageFormatListCreateInfo.viewFormatCount = (uint32_t) mergedFormats.size();
            imageFormatListCreateInfo.pViewFormats    = mergedFormats.data();

            if (appFormatList && (const void*) appFormatList == pCreateInfo->pNext)
            {
                // app's format list is the chain head: replace it with ours,
                // preserving the rest of the chain behind it
                imageFormatListCreateInfo.pNext = appFormatList->pNext;
                modifiedCreateInfo.pNext        = &imageFormatListCreateInfo;
            }
            else if (appFormatList)
            {
                // app's format list sits mid-chain in const app memory we
                // can't unlink without cloning the chain. Leave the chain
                // untouched and DON'T add a second struct -- but warn, since
                // our unorm/srgb view formats may then be missing from the
                // list the driver sees.
                Logger::warn("app format list is mid-chain; not adding ours -- srgb/unorm views may be restricted");
            }
            else
            {
                imageFormatListCreateInfo.pNext = modifiedCreateInfo.pNext;
                modifiedCreateInfo.pNext        = &imageFormatListCreateInfo;
            }
        }

        modifiedCreateInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        Logger::debug("format " + std::to_string(modifiedCreateInfo.imageFormat));
        std::shared_ptr<LogicalSwapchain> pLogicalSwapchain(new LogicalSwapchain());
        pLogicalSwapchain->pLogicalDevice      = pLogicalDevice;
        pLogicalSwapchain->swapchainCreateInfo = *pCreateInfo;
        pLogicalSwapchain->imageExtent         = modifiedCreateInfo.imageExtent;
        pLogicalSwapchain->format              = modifiedCreateInfo.imageFormat;
        pLogicalSwapchain->imageCount          = 0;

        // ONE create call, decided up front. Empirically on this system a
        // second vkCreateSwapchainKHR after a failed one deadlocks inside the
        // driver stack (build 13's stage 2 never returned), so in-process
        // retry/fallback is not an option: whatever we submit must succeed on
        // the first try.
        //
        // renderMode (vkbchoom.conf):
        //   transfer (default) -- create the swapchain with DXVK's own request
        //          (TRANSFER_DST is already in its usage, so this is
        //          byte-identical to the unlayered game, which demonstrably
        //          works). Effects render into fake images; TransferEffect
        //          copies the result into the real swapchain. SMAA fully
        //          functional, one extra copy per frame.
        //   mutable -- the modified create (mutable format + format list +
        //          SAMPLED usage, effects render directly into swapchain
        //          views). Rejected with -3 by this driver stack; kept for
        //          experimentation e.g. with overlays disabled.
        //   bypass -- exact passthrough, no effects. Pure A/B switch.
        std::string renderMode = pConfig->getOption<std::string>("renderMode", "transfer");
        Logger::info("renderMode = " + renderMode);

        // DXVK enables VK_EXT_full_screen_exclusive and chains
        // VkSurfaceFullScreenExclusiveInfoEXT (sType 1000255000) and
        // VkSurfaceFullScreenExclusiveWin32InfoEXT (sType 1000255001) into the
        // swapchain create -- on recent DXVK only to communicate
        // "disallow exclusive fullscreen". The NVIDIA driver has a documented
        // history of failing vkCreateSwapchainKHR with
        // VK_ERROR_INITIALIZATION_FAILED when this chain is present, and our
        // layered runs die with exactly that error on a create that works
        // unlayered. Stripping the two structs removes the trigger; behaviour
        // falls back to VK_FULL_SCREEN_EXCLUSIVE_DEFAULT_EXT (Windows/driver
        // decide), which is close to DXVK's intent anyway.
        //
        // Mid-chain removal requires relinking nodes that live in const app
        // memory; we const_cast, patch, and restore after the create returns.
        // Default off: stripping the full-screen-exclusive structs makes DXVK believe
        // it holds exclusive fullscreen while the driver never got the request, and
        // that mismatch breaks keyboard input at scene transitions. Only enable it if
        // swapchain creation actually fails without it.
        bool stripFse = pConfig->getOption<std::string>("stripFseInfo", "off") == "on";
        std::vector<std::pair<VkBaseInStructure*, const VkBaseInStructure*>> fsePatches;

        auto isFseStruct = [](const VkBaseInStructure* s) {
            return s->sType == VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT
                   || s->sType == VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT;
        };
        auto stripFseFromChain = [&](VkSwapchainCreateInfoKHR& info) {
            if (!stripFse)
                return;
            uint32_t stripped = 0;
            // head nodes: relink via our own (non-const) struct
            const VkBaseInStructure* head = (const VkBaseInStructure*) info.pNext;
            while (head && isFseStruct(head))
            {
                info.pNext = head->pNext;
                head       = (const VkBaseInStructure*) info.pNext;
                stripped++;
            }
            // mid-chain nodes: patch predecessor's pNext, remember for restore
            const VkBaseInStructure* prev = head;
            while (prev && prev->pNext)
            {
                const VkBaseInStructure* cur = prev->pNext;
                if (isFseStruct(cur))
                {
                    fsePatches.push_back({const_cast<VkBaseInStructure*>(prev), cur});
                    const_cast<VkBaseInStructure*>(prev)->pNext = cur->pNext;
                    stripped++;
                }
                else
                {
                    prev = cur;
                }
            }
            if (stripped)
                Logger::info("stripped " + std::to_string(stripped) + " full-screen-exclusive struct(s) from pNext chain");
        };
        auto restoreFsePatches = [&]() {
            for (auto it = fsePatches.rbegin(); it != fsePatches.rend(); ++it)
                it->first->pNext = it->second;
            fsePatches.clear();
        };

        VkResult result;

        if (renderMode == "mutable" && pLogicalDevice->supportsMutableFormat)
        {
            stripFseFromChain(modifiedCreateInfo);
            result = pLogicalDevice->vkd.CreateSwapchainKHR(device, &modifiedCreateInfo, pAllocator, pSwapchain);
            restoreFsePatches();
            Logger::info("CreateSwapchainKHR (mutable) result: " + std::to_string(result));
            if (result == VK_SUCCESS)
                pLogicalSwapchain->useMutableFormat = true;
        }
        else if (renderMode == "bypass")
        {
            VkSwapchainCreateInfoKHR bypassInfo = *pCreateInfo;
            stripFseFromChain(bypassInfo);
            result = pLogicalDevice->vkd.CreateSwapchainKHR(device, &bypassInfo, pAllocator, pSwapchain);
            restoreFsePatches();
            Logger::info("CreateSwapchainKHR (bypass) result: " + std::to_string(result));
            if (result == VK_SUCCESS)
                pLogicalSwapchain->bypass = true;
        }
        else
        {
            VkSwapchainCreateInfoKHR transferInfo = *pCreateInfo;
            transferInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            stripFseFromChain(transferInfo);
            result = pLogicalDevice->vkd.CreateSwapchainKHR(device, &transferInfo, pAllocator, pSwapchain);
            restoreFsePatches();
            Logger::info("CreateSwapchainKHR (transfer) result: " + std::to_string(result));
        }

        if (result != VK_SUCCESS)
        {
            // Deliberately NO retry -- see above. Return the error to the app.
            // If even the transfer/bypass create fails, the -3 is not about
            // our create-info contents at all: something else in the layer
            // chain (overlay layers are the usual suspect) is failing it.
            Logger::err("CreateSwapchainKHR failed (" + std::to_string(result) + "); not retrying (retry deadlocks on this driver)");
            return result;
        }

        swapchainMap[*pSwapchain] = pLogicalSwapchain;

        return result;
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkbChoom_GetSwapchainImagesKHR(VkDevice       device,
                                                                  VkSwapchainKHR swapchain,
                                                                  uint32_t*      pCount,
                                                                  VkImage*       pSwapchainImages)
    {
        scoped_lock l(globalLock);
        // info level, not trace: the old trace marker here was filtered out at
        // VKBCHOOM_LOG_LEVEL=debug, which made "GetSwapchainImagesKHR never
        // reached" impossible to distinguish from "reached but invisible".
        Logger::info(std::string("vkGetSwapchainImagesKHR (") + (pSwapchainImages ? "data" : "count") + ") pCount=" + std::to_string(*pCount));

        LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();

        if (pSwapchainImages == nullptr)
        {
            return pLogicalDevice->vkd.GetSwapchainImagesKHR(device, swapchain, pCount, pSwapchainImages);
        }

        auto scIt = swapchainMap.find(swapchain);
        if (scIt == swapchainMap.end() || !scIt->second || scIt->second->bypass)
        {
            // unknown handle (shouldn't happen now that failed creates skip the
            // map insert) or a bypass-mode swapchain: the app gets the real
            // images and we stay out of the way entirely
            return pLogicalDevice->vkd.GetSwapchainImagesKHR(device, swapchain, pCount, pSwapchainImages);
        }
        LogicalSwapchain* pLogicalSwapchain = scIt->second.get();

        // If the images got already requested once, return them again instead of creating new images
        if (pLogicalSwapchain->fakeImages.size())
        {
            *pCount = std::min<uint32_t>(*pCount, pLogicalSwapchain->imageCount);
            std::memcpy(pSwapchainImages, pLogicalSwapchain->fakeImages.data(), sizeof(VkImage) * (*pCount));
            return *pCount < pLogicalSwapchain->imageCount ? VK_INCOMPLETE : VK_SUCCESS;
        }

        pLogicalDevice->vkd.GetSwapchainImagesKHR(device, swapchain, &pLogicalSwapchain->imageCount, nullptr);
        pLogicalSwapchain->images.resize(pLogicalSwapchain->imageCount);
        pLogicalDevice->vkd.GetSwapchainImagesKHR(device, swapchain, &pLogicalSwapchain->imageCount, pLogicalSwapchain->images.data());

        std::vector<std::string> effectStrings = {"smaa"};
        if (pConfig->getOption<std::string>("rcasSharpen", "off") == "on")
            effectStrings.push_back("rcas");
        if (pConfig->getOption<std::string>("depthOutline", "off") == "on")
            effectStrings.push_back("depthoutline");
        if (pConfig->getOption<std::string>("posterize", "off") == "on")
            effectStrings.push_back("posterize");
        if (pConfig->getOption<std::string>("blackWhiteRed", "off") == "on")
            effectStrings.push_back("blackwhitered");

        // create 1 more set of images when we can't use the swapchain it self
        uint32_t fakeImageCount = pLogicalSwapchain->imageCount * (effectStrings.size() + !pLogicalSwapchain->useMutableFormat);

        pLogicalSwapchain->fakeImages =
            createFakeSwapchainImages(pLogicalDevice, pLogicalSwapchain->swapchainCreateInfo, fakeImageCount, pLogicalSwapchain->fakeImageMemory);
        Logger::debug("created fake swapchain images");

        VkFormat unormFormat = convertToUNORM(pLogicalSwapchain->format);
        VkFormat srgbFormat  = convertToSRGB(pLogicalSwapchain->format);

        for (uint32_t i = 0; i < effectStrings.size(); i++)
        {
            Logger::debug("current effectString " + effectStrings[i]);
            std::vector<VkImage> firstImages(pLogicalSwapchain->fakeImages.begin() + pLogicalSwapchain->imageCount * i,
                                             pLogicalSwapchain->fakeImages.begin() + pLogicalSwapchain->imageCount * (i + 1));
            Logger::debug(std::to_string(firstImages.size()) + " images in firstImages");
            std::vector<VkImage> secondImages;
            if (i == effectStrings.size() - 1)
            {
                secondImages = pLogicalSwapchain->useMutableFormat
                                   ? pLogicalSwapchain->images
                                   : std::vector<VkImage>(pLogicalSwapchain->fakeImages.end() - pLogicalSwapchain->imageCount,
                                                          pLogicalSwapchain->fakeImages.end());
                Logger::debug("using swapchain images as second images");
            }
            else
            {
                secondImages = std::vector<VkImage>(pLogicalSwapchain->fakeImages.begin() + pLogicalSwapchain->imageCount * (i + 1),
                                                    pLogicalSwapchain->fakeImages.begin() + pLogicalSwapchain->imageCount * (i + 2));
                Logger::debug("not using swapchain images as second images");
            }
            Logger::debug(std::to_string(secondImages.size()) + " images in secondImages");
            if (effectStrings[i] == "rcas")
            {
                pLogicalSwapchain->effects.push_back(std::shared_ptr<Effect>(
                    new RcasEffect(pLogicalDevice, unormFormat, pLogicalSwapchain->imageExtent, firstImages, secondImages, pConfig.get())));
                Logger::info("RCAS sharpen stage initialized, effect pipeline created");
            }
            else if (effectStrings[i] == "depthoutline")
            {
                pLogicalSwapchain->effects.push_back(std::shared_ptr<Effect>(new DepthOutlineEffect(
                    pLogicalDevice, unormFormat, pLogicalSwapchain->imageExtent, firstImages, secondImages, pConfig.get())));
                Logger::info("depth outline stage initialized, effect pipeline created");
            }
            else if (effectStrings[i] == "posterize")
            {
                pLogicalSwapchain->effects.push_back(std::shared_ptr<Effect>(new PosterizeEffect(
                    pLogicalDevice, unormFormat, pLogicalSwapchain->imageExtent, firstImages, secondImages, pConfig.get())));
                Logger::info("posterize stage initialized, effect pipeline created");
            }
            else if (effectStrings[i] == "blackwhitered")
            {
                pLogicalSwapchain->effects.push_back(std::shared_ptr<Effect>(
                    new BwrEffect(pLogicalDevice, unormFormat, pLogicalSwapchain->imageExtent, firstImages, secondImages, pConfig.get())));
                Logger::info("black/white/red stage initialized, effect pipeline created");
            }
            else
            {
                pLogicalSwapchain->effects.push_back(std::shared_ptr<Effect>(
                    new SmaaEffect(pLogicalDevice, unormFormat, pLogicalSwapchain->imageExtent, firstImages, secondImages, pConfig.get())));
                Logger::info("SMAA layer initialized, effect pipeline created");
            }
        }

        if (!pLogicalSwapchain->useMutableFormat)
        {
            pLogicalSwapchain->effects.push_back(std::shared_ptr<Effect>(new TransferEffect(
                pLogicalDevice,
                pLogicalSwapchain->format,
                pLogicalSwapchain->imageExtent,
                std::vector<VkImage>(pLogicalSwapchain->fakeImages.end() - pLogicalSwapchain->imageCount, pLogicalSwapchain->fakeImages.end()),
                pLogicalSwapchain->images,
                pConfig.get())));
        }

        DepthMatch depthMatch      = findMatchingDepthImage(pLogicalDevice, pLogicalSwapchain->imageExtent);
        VkImageView depthImageView = depthMatch.view;
        VkImage     depthImage     = depthMatch.image;
        VkFormat    depthFormat    = depthMatch.format;

        Logger::debug("effect string count: " + std::to_string(effectStrings.size()));
        Logger::debug("effect count: " + std::to_string(pLogicalSwapchain->effects.size()));

        pLogicalSwapchain->commandBuffersEffect = allocateCommandBuffer(pLogicalDevice, pLogicalSwapchain->imageCount);
        Logger::debug("allocated ComandBuffers " + std::to_string(pLogicalSwapchain->commandBuffersEffect.size()) + " for swapchain "
                      + convertToString(swapchain));

        writeCommandBuffers(
            pLogicalDevice, pLogicalSwapchain->effects, depthImage, depthImageView, depthFormat, pLogicalSwapchain->commandBuffersEffect);
        Logger::debug("wrote CommandBuffers");

        pLogicalSwapchain->semaphores = createSemaphores(pLogicalDevice, pLogicalSwapchain->imageCount);
        Logger::debug("created semaphores");
        for (unsigned int i = 0; i < pLogicalSwapchain->imageCount; i++)
        {
            Logger::debug(std::to_string(i) + " written commandbuffer " + convertToString(pLogicalSwapchain->commandBuffersEffect[i]));
        }
        Logger::trace("vkGetSwapchainImagesKHR");

        pLogicalSwapchain->defaultTransfer = std::shared_ptr<Effect>(new TransferEffect(
            pLogicalDevice,
            pLogicalSwapchain->format,
            pLogicalSwapchain->imageExtent,
            std::vector<VkImage>(pLogicalSwapchain->fakeImages.begin(), pLogicalSwapchain->fakeImages.begin() + pLogicalSwapchain->imageCount),
            pLogicalSwapchain->images,
            pConfig.get()));

        pLogicalSwapchain->commandBuffersNoEffect = allocateCommandBuffer(pLogicalDevice, pLogicalSwapchain->imageCount);

        writeCommandBuffers(pLogicalDevice,
                            {pLogicalSwapchain->defaultTransfer},
                            VK_NULL_HANDLE,
                            VK_NULL_HANDLE,
                            VK_FORMAT_UNDEFINED,
                            pLogicalSwapchain->commandBuffersNoEffect);

        for (unsigned int i = 0; i < pLogicalSwapchain->imageCount; i++)
        {
            Logger::debug(std::to_string(i) + " written commandbuffer " + convertToString(pLogicalSwapchain->commandBuffersNoEffect[i]));
        }

        *pCount = std::min<uint32_t>(*pCount, pLogicalSwapchain->imageCount);
        std::memcpy(pSwapchainImages, pLogicalSwapchain->fakeImages.data(), sizeof(VkImage) * (*pCount));
        return *pCount < pLogicalSwapchain->imageCount ? VK_INCOMPLETE : VK_SUCCESS;
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkbChoom_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
    {
        scoped_lock l(globalLock);

        // One-shot marker: previously this function had NO entry logging at
        // all, so "never reached vkQueuePresentKHR" rested entirely on the F9
        // toggle line not appearing. Make it unambiguous without per-frame spam.
        static bool firstPresentLogged = false;
        if (!firstPresentLogged)
        {
            Logger::info("first vkQueuePresentKHR reached -- present chain is hooked");
            firstPresentLogged = true;
        }

        // SMAA is always on. There is no runtime toggle.
        //
        // There used to be an F9 A/B switch here. It worked -- it selected
        // between two genuinely different sets of recorded GPU commands -- but
        // SMAA at sane settings is subtle enough that you cannot tell by eye
        // which state you are in. That makes a live hotkey a liability: one
        // stray keypress and the rest of the session runs with no
        // anti-aliasing and no indication of it. Removed deliberately.
        //
        // To A/B test, set "renderMode = bypass" in vkbchoom.conf and relaunch.
        // That leaves the layer loaded but passes frames through untouched.

        LogicalDevice* pLogicalDevice = deviceMap[GetKey(queue)].get();

        std::vector<VkSemaphore> presentSemaphores;
        presentSemaphores.reserve(pPresentInfo->swapchainCount);

        std::vector<VkPipelineStageFlags> waitStages(pPresentInfo->waitSemaphoreCount, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

        // Populated by the first non-bypass swapchain in the loop below, for
        // an F11 screenshot capture after all of this frame's effect work
        // has been submitted (see the capture call after the loop). Screen-
        // shots only cover the first swapchain in a multi-swapchain present
        // (an edge case outside normal single-monitor play).
        bool       screenshotTargetSet    = false;
        VkImage    screenshotTargetImage  = VK_NULL_HANDLE;
        VkFormat   screenshotTargetFormat = VK_FORMAT_UNDEFINED;
        VkExtent2D screenshotTargetExtent = {};

        for (unsigned int i = 0; i < (*pPresentInfo).swapchainCount; i++)
        {
            uint32_t       index     = (*pPresentInfo).pImageIndices[i];
            VkSwapchainKHR swapchain = (*pPresentInfo).pSwapchains[i];

            auto scIt = swapchainMap.find(swapchain);
            if (scIt == swapchainMap.end() || !scIt->second || scIt->second->bypass)
            {
                // bypass-mode (or unknown) swapchain: nothing of ours to
                // submit for it. If it's the first swapchain, the app's wait
                // semaphores stay in the final present below.
                static bool bypassLogged = false;
                if (!bypassLogged)
                {
                    Logger::info("QueuePresentKHR: swapchain in bypass mode, presenting untouched");
                    bypassLogged = true;
                }
                continue;
            }
            LogicalSwapchain* pLogicalSwapchain = scIt->second.get();

            if (!screenshotTargetSet)
            {
                screenshotTargetImage  = pLogicalSwapchain->images[index];
                screenshotTargetFormat = pLogicalSwapchain->format;
                screenshotTargetExtent = pLogicalSwapchain->imageExtent;
                screenshotTargetSet    = true;
            }

            for (auto& effect : pLogicalSwapchain->effects)
            {
                effect->updateEffect();
            }

            // Always the effect command buffer. commandBuffersNoEffect is still
            // recorded at swapchain creation and is what "renderMode = bypass"
            // routes through, but the normal path no longer selects it.
            VkCommandBuffer chosen = pLogicalSwapchain->commandBuffersEffect[index];

            // One-shot confirmation that the SMAA path is the one running --
            // per swapchain (see LogicalSwapchain::submitLogged), so a
            // recreated swapchain gets its own line instead of relying on
            // whatever the first swapchain in the process already logged.
            if (!pLogicalSwapchain->submitLogged)
            {
                Logger::info("submitting SMAA command buffer " + convertToString(chosen) + " -- effect is active (swapchain "
                             + convertToString(swapchain) + ")");
                pLogicalSwapchain->submitLogged = true;
            }

            VkSubmitInfo submitInfo;
            submitInfo.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.pNext                = nullptr;
            submitInfo.waitSemaphoreCount   = i == 0 ? pPresentInfo->waitSemaphoreCount : 0;
            submitInfo.pWaitSemaphores      = i == 0 ? pPresentInfo->pWaitSemaphores : nullptr;
            submitInfo.pWaitDstStageMask    = i == 0 ? waitStages.data() : nullptr;
            submitInfo.commandBufferCount   = 1;
            submitInfo.pCommandBuffers      = &chosen;
            submitInfo.signalSemaphoreCount = 1;
            submitInfo.pSignalSemaphores    = &(pLogicalSwapchain->semaphores[index]);

            presentSemaphores.push_back(pLogicalSwapchain->semaphores[index]);

            VkResult vr = pLogicalDevice->vkd.QueueSubmit(pLogicalDevice->queue, 1, &submitInfo, VK_NULL_HANDLE);

            if (vr != VK_SUCCESS)
            {
                return vr;
            }
        }

        VkPresentInfoKHR presentInfo = *pPresentInfo;
        if (!presentSemaphores.empty())
        {
            // we consumed the app's wait semaphores in our submits, so the
            // present must wait on ours instead
            presentInfo.waitSemaphoreCount = presentSemaphores.size();
            presentInfo.pWaitSemaphores    = presentSemaphores.data();
        }
        // else: every swapchain in this present was bypass -- leave the app's
        // own wait semaphores in place, fully untouched present

        // Deliberately placed after every effect submission for this frame
        // and before the real present: captureScreenshot's own QueueWaitIdle
        // guarantees this frame's effect work (submitted above, same queue)
        // has actually finished before it reads the image, and running
        // before the real present means the image is still in
        // PRESENT_SRC_KHR layout, matching what captureScreenshot expects.
        // GetAsyncKeyState is a read-only poll, not a hook -- this cannot
        // affect the game's own input handling either way.
        if (screenshotTargetSet && wasScreenshotKeyJustPressed(pConfig.get()))
        {
            captureScreenshot(pLogicalDevice, screenshotTargetImage, screenshotTargetFormat, screenshotTargetExtent);
        }

        return pLogicalDevice->vkd.QueuePresentKHR(queue, &presentInfo);
    }

    VKAPI_ATTR void VKAPI_CALL vkbChoom_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator)
    {
        if (!swapchain)
            return;

        scoped_lock l(globalLock);
        // we need to delete the infos of the oldswapchain

        Logger::trace("vkDestroySwapchainKHR " + convertToString(swapchain));
        auto scIt = swapchainMap.find(swapchain);
        if (scIt != swapchainMap.end())
        {
            if (scIt->second)
                scIt->second->destroy();
            swapchainMap.erase(scIt);
        }
        LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();

        pLogicalDevice->vkd.DestroySwapchainKHR(device, swapchain, pAllocator);
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkbChoom_CreateImage(VkDevice                     device,
                                                        const VkImageCreateInfo*     pCreateInfo,
                                                        const VkAllocationCallbacks* pAllocator,
                                                        VkImage*                     pImage)
    {
        scoped_lock l(globalLock);

        LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();
        if (isDepthFormat(pCreateInfo->format) && pCreateInfo->samples == VK_SAMPLE_COUNT_1_BIT
            && ((pCreateInfo->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) == VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))
        {
            Logger::info("detected depth image #" + std::to_string(pLogicalDevice->depthImages.size() + 1) + ": format="
                         + convertToString(pCreateInfo->format) + " " + std::to_string(pCreateInfo->extent.width) + "x"
                         + std::to_string(pCreateInfo->extent.height));

            VkImageCreateInfo modifiedCreateInfo = *pCreateInfo;
            modifiedCreateInfo.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
            VkResult result = pLogicalDevice->vkd.CreateImage(device, &modifiedCreateInfo, pAllocator, pImage);
            pLogicalDevice->depthImages.push_back(*pImage);
            pLogicalDevice->depthFormats.push_back(pCreateInfo->format);
            pLogicalDevice->depthExtents.push_back({pCreateInfo->extent.width, pCreateInfo->extent.height});

            return result;
        }
        else
        {
            return pLogicalDevice->vkd.CreateImage(device, pCreateInfo, pAllocator, pImage);
        }
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkbChoom_BindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory, VkDeviceSize memoryOffset)
    {
        scoped_lock l(globalLock);

        LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();

        VkResult result = pLogicalDevice->vkd.BindImageMemory(device, image, memory, memoryOffset);
        // TODO what if the application creates more than one image before binding memory?
        if (pLogicalDevice->depthImages.size() && image == pLogicalDevice->depthImages.back())
        {
            Logger::debug("before creating depth image view");
            VkImageView depthImageView = createImageViews(pLogicalDevice,
                                                          pLogicalDevice->depthFormats[pLogicalDevice->depthImages.size() - 1],
                                                          {image},
                                                          VK_IMAGE_VIEW_TYPE_2D,
                                                          VK_IMAGE_ASPECT_DEPTH_BIT)[0];

            VkFormat   depthFormat = pLogicalDevice->depthFormats[pLogicalDevice->depthImages.size() - 1];
            VkExtent2D depthExtent = pLogicalDevice->depthExtents[pLogicalDevice->depthImages.size() - 1];

            Logger::debug("created depth image view");
            pLogicalDevice->depthImageViews.push_back(depthImageView);

            // Resolution match against every currently active swapchain,
            // not just "is this the very first depth image ever seen" --
            // see findMatchingDepthImage for why.
            bool wiredAny = false;
            for (auto& it : swapchainMap)
            {
                LogicalSwapchain* pLogicalSwapchain = it.second.get();
                if (pLogicalSwapchain->pLogicalDevice == pLogicalDevice && pLogicalSwapchain->imageExtent.width == depthExtent.width
                    && pLogicalSwapchain->imageExtent.height == depthExtent.height)
                {
                    if (pLogicalSwapchain->commandBuffersEffect.size())
                    {
                        pLogicalDevice->vkd.FreeCommandBuffers(pLogicalDevice->device,
                                                               pLogicalDevice->commandPool,
                                                               pLogicalSwapchain->commandBuffersEffect.size(),
                                                               pLogicalSwapchain->commandBuffersEffect.data());
                        pLogicalSwapchain->commandBuffersEffect.clear();
                        pLogicalSwapchain->commandBuffersEffect = allocateCommandBuffer(pLogicalDevice, pLogicalSwapchain->imageCount);
                        Logger::debug("allocated CommandBuffers for swapchain " + convertToString(it.first));

                        writeCommandBuffers(
                            pLogicalDevice, pLogicalSwapchain->effects, image, depthImageView, depthFormat, pLogicalSwapchain->commandBuffersEffect);
                        Logger::debug("wrote CommandBuffers");
                        wiredAny = true;
                    }
                }
            }

            std::string sizeStr = std::to_string(depthExtent.width) + "x" + std::to_string(depthExtent.height);
            if (wiredAny)
                Logger::info("depth image #" + std::to_string(pLogicalDevice->depthImageViews.size()) + " (" + sizeStr
                             + ") matches an active swapchain's resolution -- effects switched to it");
            else
                Logger::info("depth image #" + std::to_string(pLogicalDevice->depthImageViews.size()) + " (" + sizeStr
                             + ") bound, but doesn't match any active swapchain's resolution -- not switched to");
        }
        return result;
    }

    VKAPI_ATTR void VKAPI_CALL vkbChoom_DestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks* pAllocator)
    {
        if (!image)
            return;

        scoped_lock l(globalLock);

        LogicalDevice* pLogicalDevice = deviceMap[GetKey(device)].get();

        for (uint32_t i = 0; i < pLogicalDevice->depthImages.size(); i++)
        {
            if (pLogicalDevice->depthImages[i] == image)
            {
                pLogicalDevice->depthImages.erase(pLogicalDevice->depthImages.begin() + i);
                // TODO what if a image gets destroyed before binding memory?
                if (pLogicalDevice->depthImageViews.size() - 1 >= i)
                {
                    pLogicalDevice->vkd.DestroyImageView(pLogicalDevice->device, pLogicalDevice->depthImageViews[i], nullptr);
                    pLogicalDevice->depthImageViews.erase(pLogicalDevice->depthImageViews.begin() + i);
                }
                pLogicalDevice->depthFormats.erase(pLogicalDevice->depthFormats.begin() + i);
                pLogicalDevice->depthExtents.erase(pLogicalDevice->depthExtents.begin() + i);

                // Each swapchain gets its own resolution-matched re-pick here
                // rather than one shared pick applied to all of them --
                // different active swapchains (e.g. the splash swapchain
                // briefly alongside the real one) can legitimately want
                // different depth images.
                for (auto& it : swapchainMap)
                {
                    LogicalSwapchain* pLogicalSwapchain = it.second.get();
                    if (pLogicalSwapchain->pLogicalDevice == pLogicalDevice)
                    {
                        if (pLogicalSwapchain->commandBuffersEffect.size())
                        {
                            DepthMatch depthMatch = findMatchingDepthImage(pLogicalDevice, pLogicalSwapchain->imageExtent);

                            pLogicalDevice->vkd.FreeCommandBuffers(pLogicalDevice->device,
                                                                   pLogicalDevice->commandPool,
                                                                   pLogicalSwapchain->commandBuffersEffect.size(),
                                                                   pLogicalSwapchain->commandBuffersEffect.data());
                            pLogicalSwapchain->commandBuffersEffect.clear();
                            pLogicalSwapchain->commandBuffersEffect = allocateCommandBuffer(pLogicalDevice, pLogicalSwapchain->imageCount);
                            Logger::debug("allocated CommandBuffers for swapchain " + convertToString(it.first));

                            writeCommandBuffers(pLogicalDevice,
                                                pLogicalSwapchain->effects,
                                                depthMatch.image,
                                                depthMatch.view,
                                                depthMatch.format,
                                                pLogicalSwapchain->commandBuffersEffect);
                            Logger::debug("wrote CommandBuffers");
                        }
                    }
                }
            }
        }

        pLogicalDevice->vkd.DestroyImage(pLogicalDevice->device, image, pAllocator);
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // Enumeration function

    VkResult VKAPI_CALL vkbChoom_EnumerateInstanceLayerProperties(uint32_t* pPropertyCount, VkLayerProperties* pProperties)
    {
        if (pPropertyCount)
            *pPropertyCount = 1;

        if (pProperties)
        {
            std::strcpy(pProperties->layerName, VKBCHOOM_NAME);
            std::strcpy(pProperties->description, "a post processing layer");
            pProperties->implementationVersion = 1;
            pProperties->specVersion           = VK_MAKE_VERSION(1, 2, 0);
        }

        return VK_SUCCESS;
    }

    VkResult VKAPI_CALL vkbChoom_EnumerateDeviceLayerProperties(VkPhysicalDevice   physicalDevice,
                                                                uint32_t*          pPropertyCount,
                                                                VkLayerProperties* pProperties)
    {
        return vkbChoom_EnumerateInstanceLayerProperties(pPropertyCount, pProperties);
    }

    VkResult VKAPI_CALL vkbChoom_EnumerateInstanceExtensionProperties(const char*            pLayerName,
                                                                      uint32_t*              pPropertyCount,
                                                                      VkExtensionProperties* pProperties)
    {
        if (pLayerName == NULL || std::strcmp(pLayerName, VKBCHOOM_NAME))
        {
            return VK_ERROR_LAYER_NOT_PRESENT;
        }

        // don't expose any extensions
        if (pPropertyCount)
        {
            *pPropertyCount = 0;
        }
        return VK_SUCCESS;
    }

    VkResult VKAPI_CALL vkbChoom_EnumerateDeviceExtensionProperties(VkPhysicalDevice       physicalDevice,
                                                                    const char*            pLayerName,
                                                                    uint32_t*              pPropertyCount,
                                                                    VkExtensionProperties* pProperties)
    {
        // pass through any queries that aren't to us
        if (pLayerName == NULL || std::strcmp(pLayerName, VKBCHOOM_NAME))
        {
            if (physicalDevice == VK_NULL_HANDLE)
            {
                return VK_SUCCESS;
            }

            scoped_lock l(globalLock);
            return instanceDispatchMap[GetKey(physicalDevice)].EnumerateDeviceExtensionProperties(
                physicalDevice, pLayerName, pPropertyCount, pProperties);
        }

        // don't expose any extensions
        if (pPropertyCount)
        {
            *pPropertyCount = 0;
        }
        return VK_SUCCESS;
    }
} // namespace vkbChoom

extern "C"
{ // these are the entry points for the layer, so they need to be c-linkeable

    VKBCHOOM_EXPORT PFN_vkVoidFunction VKAPI_CALL vkbChoom_GetDeviceProcAddr(VkDevice device, const char* pName);
    VKBCHOOM_EXPORT PFN_vkVoidFunction VKAPI_CALL vkbChoom_GetInstanceProcAddr(VkInstance instance, const char* pName);

    // Interface v2 loaders look for this exported symbol BEFORE falling back
    // to the named vkGetInstanceProcAddr/vkGetDeviceProcAddr lookup the
    // manifest's "functions" block declares. If a sufficiently new loader
    // requires negotiation and doesn't find this export, it can skip the
    // layer entirely -- silently, same failure signature as every other
    // dead end in this investigation (game runs fine, zero log, no error).
    // This was never present in this codebase; adding it is strictly
    // additive and doesn't touch the proven-working GetInstanceProcAddr /
    // GetDeviceProcAddr implementations below, it just gives the loader a
    // second, more modern way to find them.
    // NOTE: no VKBCHOOM_EXPORT here. vulkan/vk_layer.h already declares this
    // exact function (plain, no dllexport) inside its own extern "C" block --
    // that's how we get VkNegotiateLayerInterface's type checked against the
    // real prototype. Redeclaring it here with __declspec(dllexport) makes
    // MSVC see two conflicting declarations of the same symbol ("different
    // linkage", C2375) since dllexport counts as part of linkage for this
    // check, even though the extern "C" language linkage matches fine. The
    // definition below matches the header's declaration exactly instead, and
    // we export the symbol via a linker pragma below -- which acts at link
    // time, not as a second C++ declaration, so it can't conflict this way.
    VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct)
    {
        if (pVersionStruct == nullptr || pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT)
            return VK_ERROR_INITIALIZATION_FAILED;

        // We only need interface v1 behavior (get*ProcAddr by name); report
        // the lower of what we support and what the loader asked for rather
        // than unconditionally claiming v2, per the negotiation contract.
        if (pVersionStruct->loaderLayerInterfaceVersion > CURRENT_LOADER_LAYER_INTERFACE_VERSION)
            pVersionStruct->loaderLayerInterfaceVersion = CURRENT_LOADER_LAYER_INTERFACE_VERSION;

        pVersionStruct->pfnGetInstanceProcAddr     = vkbChoom_GetInstanceProcAddr;
        pVersionStruct->pfnGetDeviceProcAddr       = vkbChoom_GetDeviceProcAddr;
        pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr; // we don't intercept any GetPhysicalDevice* calls

        vkbChoom::breadcrumb("vkNegotiateLoaderLayerInterfaceVersion  loader asked for v"
                             + std::to_string(pVersionStruct->loaderLayerInterfaceVersion));

        vkbChoom::Logger::info("vkNegotiateLoaderLayerInterfaceVersion called by loader -- negotiated interface v"
                                + std::to_string(pVersionStruct->loaderLayerInterfaceVersion));

        return VK_SUCCESS;
    }

#if defined(_WIN32)
    // MSVC: since the function above deliberately has no __declspec(dllexport)
    // (see the comment above its definition), export it at the linker level
    // instead so it still shows up in the DLL's export table.
#pragma comment(linker, "/export:vkNegotiateLoaderLayerInterfaceVersion")
#endif

#define GETPROCADDR(func) \
    if (!std::strcmp(pName, "vk" #func)) \
        return (PFN_vkVoidFunction) &vkbChoom::vkbChoom_##func;
    /*
    Return our funktions for the funktions we want to intercept
    the macro takes the name and returns our vkbChoom_##func, if the name is equal
    */

    // vkGetDeviceProcAddr needs to behave like vkGetInstanceProcAddr thanks to some games
#define INTERCEPT_CALLS \
    if (vkbChoom::isTargetProcess()) \
    { \
        /* instance chain functions we intercept */ \
        if (!std::strcmp(pName, "vkGetInstanceProcAddr")) \
            return (PFN_vkVoidFunction) &vkbChoom_GetInstanceProcAddr; \
        GETPROCADDR(EnumerateInstanceLayerProperties); \
        GETPROCADDR(EnumerateInstanceExtensionProperties); \
        GETPROCADDR(CreateInstance); \
        GETPROCADDR(DestroyInstance); \
\
        /* device chain functions we intercept*/ \
        if (!std::strcmp(pName, "vkGetDeviceProcAddr")) \
            return (PFN_vkVoidFunction) &vkbChoom_GetDeviceProcAddr; \
        GETPROCADDR(EnumerateDeviceLayerProperties); \
        GETPROCADDR(EnumerateDeviceExtensionProperties); \
        GETPROCADDR(CreateDevice); \
        GETPROCADDR(DestroyDevice); \
        GETPROCADDR(CreateSwapchainKHR); \
        GETPROCADDR(GetSwapchainImagesKHR); \
        GETPROCADDR(QueuePresentKHR); \
        GETPROCADDR(DestroySwapchainKHR); \
\
        if (vkbChoom::pConfig->getOption<std::string>("depthCapture", "off") == "on") \
        { \
            GETPROCADDR(CreateImage); \
            GETPROCADDR(DestroyImage); \
            GETPROCADDR(BindImageMemory); \
        } \
    }

    VKBCHOOM_EXPORT PFN_vkVoidFunction VKAPI_CALL vkbChoom_GetDeviceProcAddr(VkDevice device, const char* pName)
    {
        if (vkbChoom::pConfig == nullptr)
        {
            vkbChoom::pConfig = std::shared_ptr<vkbChoom::Config>(new vkbChoom::Config());
        }

        if (pName == nullptr)
            return nullptr;

        vkbChoom::Logger::trace(std::string("GetDeviceProcAddr queried: ") + pName);

        INTERCEPT_CALLS

        {
            vkbChoom::scoped_lock l(vkbChoom::globalLock);
            // find(), not operator[]: operator[] on a miss default-constructs
            // a null shared_ptr in the map and the -> below becomes a null
            // deref (a crash with no log, i.e. exactly the failure mode being
            // debugged). Also guard device == VK_NULL_HANDLE, since GetKey()
            // dereferences the handle.
            if (device == VK_NULL_HANDLE)
                return nullptr;
            auto it = vkbChoom::deviceMap.find(vkbChoom::GetKey(device));
            if (it == vkbChoom::deviceMap.end() || !it->second || !it->second->vkd.GetDeviceProcAddr)
            {
                vkbChoom::Logger::warn(std::string("GetDeviceProcAddr fallthrough with unknown device for: ") + pName);
                return nullptr;
            }
            return it->second->vkd.GetDeviceProcAddr(device, pName);
        }
    }

    VKBCHOOM_EXPORT PFN_vkVoidFunction VKAPI_CALL vkbChoom_GetInstanceProcAddr(VkInstance instance, const char* pName)
    {
        // Proof that the loader reached our code, independent of whether the
        // config or the log file resolved. Fires once per process.
        static bool s_firstCall = true;
        if (s_firstCall)
        {
            s_firstCall = false;
            vkbChoom::breadcrumb(std::string("first vkbChoom_GetInstanceProcAddr -- loader is in our chain (pName=")
                                 + (pName ? pName : "<null>") + ")");
        }

        if (vkbChoom::pConfig == nullptr)
        {
            vkbChoom::pConfig = std::shared_ptr<vkbChoom::Config>(new vkbChoom::Config());
        }

        if (pName == nullptr)
            return nullptr;

        vkbChoom::Logger::trace(std::string("GetInstanceProcAddr queried: ") + pName);

        INTERCEPT_CALLS

        {
            vkbChoom::scoped_lock l(vkbChoom::globalLock);
            // The loader is allowed to query with instance == NULL for global
            // functions; GetKey() dereferences the handle, so guard it, and
            // use find() to avoid inserting an empty dispatch on a miss.
            if (instance == VK_NULL_HANDLE)
                return nullptr;
            auto it = vkbChoom::instanceDispatchMap.find(vkbChoom::GetKey(instance));
            if (it == vkbChoom::instanceDispatchMap.end() || !it->second.GetInstanceProcAddr)
            {
                vkbChoom::Logger::warn(std::string("GetInstanceProcAddr fallthrough with unknown instance for: ") + pName);
                return nullptr;
            }
            return it->second.GetInstanceProcAddr(instance, pName);
        }
    }

} // extern "C"
