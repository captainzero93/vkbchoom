#include "screenshot.hpp"

#include "buffer.hpp"
#include "platform_win32.hpp"
#include "logger.hpp"
#include "util.hpp"

#include <windows.h>
#include <cstdio>
#include <fstream>
#include <vector>
#include <unordered_map>

namespace vkbChoom
{
    // Small set of keys that make sense as a screenshot hotkey -- not every
    // possible VK_ code, just the common, easily-reachable ones. Unknown
    // names fall back to F11.
    static int keyNameToVirtualKey(const std::string& name)
    {
        static const std::unordered_map<std::string, int> table = {
            {"F1", VK_F1},
            {"F2", VK_F2},
            {"F3", VK_F3},
            {"F4", VK_F4},
            {"F5", VK_F5},
            {"F6", VK_F6},
            {"F7", VK_F7},
            {"F8", VK_F8},
            {"F9", VK_F9},
            {"F10", VK_F10},
            {"F11", VK_F11},
            {"F12", VK_F12},
            {"PrintScreen", VK_SNAPSHOT},
            {"ScrollLock", VK_SCROLL},
            {"Pause", VK_PAUSE},
            {"Insert", VK_INSERT},
            {"Home", VK_HOME},
            {"End", VK_END},
            {"PageUp", VK_PRIOR},
            {"PageDown", VK_NEXT},
        };
        auto it = table.find(name);
        if (it != table.end())
            return it->second;

        Logger::warn("screenshot: unrecognised screenshotKey \"" + name + "\", using F11");
        return VK_F11;
    }

    bool wasScreenshotKeyJustPressed(Config* pConfig)
    {
        // Resolved once and cached rather than looked up every frame --
        // pConfig->getOption does string work on every call, and this only
        // needs to happen once since the config is only read at launch
        // anyway (same as everything else in vkbchoom.conf).
        static int  virtualKey = keyNameToVirtualKey(pConfig->getOption<std::string>("screenshotKey", "F11"));
        static bool wasDown    = false;

        bool isDown      = (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
        bool justPressed = isDown && !wasDown;
        wasDown          = isDown;
        return justPressed;
    }

#pragma pack(push, 1)
    struct BmpFileHeader
    {
        uint16_t type       = 0x4D42; // 'BM'
        uint32_t fileSize;
        uint16_t reserved1  = 0;
        uint16_t reserved2  = 0;
        uint32_t dataOffset = 54; // sizeof(BmpFileHeader) + sizeof(BmpInfoHeader)
    };

    struct BmpInfoHeader
    {
        uint32_t headerSize      = 40;
        int32_t  width;
        int32_t  height; // positive = bottom-up row order, standard BMP convention
        uint16_t planes          = 1;
        uint16_t bitsPerPixel    = 24;
        uint32_t compression     = 0; // BI_RGB
        uint32_t imageSize       = 0; // can be 0 for BI_RGB
        int32_t  xPixelsPerMeter = 0;
        int32_t  yPixelsPerMeter = 0;
        uint32_t colorsUsed      = 0;
        uint32_t importantColors = 0;
    };
#pragma pack(pop)

    static std::string timestampedFilename()
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        char buf[64];
        sprintf_s(buf, sizeof(buf), "vkbchoom_%04d%02d%02d_%02d%02d%02d.bmp", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        return std::string(buf);
    }

    void captureScreenshot(LogicalDevice* pLogicalDevice, VkImage image, VkFormat format, VkExtent2D extent)
    {
        if (format != VK_FORMAT_R8G8B8A8_SRGB && format != VK_FORMAT_R8G8B8A8_UNORM)
        {
            Logger::warn("screenshot: swapchain format " + convertToString(format)
                         + " is not R8G8B8A8 -- skipping capture rather than write a wrong-coloured file");
            return;
        }

        Logger::info("screenshot: hotkey pressed, capturing " + std::to_string(extent.width) + "x" + std::to_string(extent.height));

        VkDeviceSize srcSize = static_cast<VkDeviceSize>(extent.width) * extent.height * 4; // R8G8B8A8, 4 bytes/pixel

        VkBuffer       stagingBuffer;
        VkDeviceMemory stagingMemory;
        createBuffer(pLogicalDevice,
                    srcSize,
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    stagingBuffer,
                    stagingMemory);

        // Same one-shot command buffer shape used elsewhere in this project
        // (see image.cpp) -- allocate, init its dispatch table (it's a
        // dispatchable object), record, submit, QueueWaitIdle. The
        // QueueWaitIdle here is what guarantees this frame's own effect
        // chain (submitted immediately before this, on the same queue) has
        // actually finished writing the image before this copy reads it --
        // same-queue submission order plus waiting on the later submission
        // transitively covers the earlier one.
        VkCommandBufferAllocateInfo allocInfo = {};
        allocInfo.sType                       = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.level                       = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandPool                 = pLogicalDevice->commandPool;
        allocInfo.commandBufferCount          = 1;

        VkCommandBuffer commandBuffer;
        pLogicalDevice->vkd.AllocateCommandBuffers(pLogicalDevice->device, &allocInfo, &commandBuffer);
        initializeDispatchTable(commandBuffer, pLogicalDevice->device);

        VkCommandBufferBeginInfo beginInfo = {};
        beginInfo.sType                    = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags                    = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        pLogicalDevice->vkd.BeginCommandBuffer(commandBuffer, &beginInfo);

        VkImageMemoryBarrier barrier;
        barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.pNext                           = nullptr;
        barrier.srcAccessMask                   = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask                   = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout                       = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.newLayout                       = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barrier.image                           = image;
        barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel   = 0;
        barrier.subresourceRange.levelCount     = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount     = 1;

        pLogicalDevice->vkd.CmdPipelineBarrier(
            commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

        VkBufferImageCopy region;
        region.bufferOffset                    = 0;
        region.bufferRowLength                 = 0;
        region.bufferImageHeight               = 0;
        region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel       = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount     = 1;
        region.imageOffset                     = {0, 0, 0};
        region.imageExtent                     = {extent.width, extent.height, 1};

        pLogicalDevice->vkd.CmdCopyImageToBuffer(commandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stagingBuffer, 1, &region);

        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = 0;
        barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        pLogicalDevice->vkd.CmdPipelineBarrier(
            commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

        pLogicalDevice->vkd.EndCommandBuffer(commandBuffer);

        VkSubmitInfo submitInfo       = {};
        submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers    = &commandBuffer;

        pLogicalDevice->vkd.QueueSubmit(pLogicalDevice->queue, 1, &submitInfo, VK_NULL_HANDLE);
        pLogicalDevice->vkd.QueueWaitIdle(pLogicalDevice->queue);

        pLogicalDevice->vkd.FreeCommandBuffers(pLogicalDevice->device, pLogicalDevice->commandPool, 1, &commandBuffer);

        void* mapped;
        pLogicalDevice->vkd.MapMemory(pLogicalDevice->device, stagingMemory, 0, srcSize, 0, &mapped);
        const uint8_t* src = static_cast<const uint8_t*>(mapped);

        uint32_t             rowPaddedSize = ((extent.width * 3 + 3) / 4) * 4;
        std::vector<uint8_t> row(rowPaddedSize, 0);

        BmpFileHeader fileHeader;
        BmpInfoHeader infoHeader;
        infoHeader.width    = static_cast<int32_t>(extent.width);
        infoHeader.height   = static_cast<int32_t>(extent.height);
        fileHeader.fileSize = fileHeader.dataOffset + rowPaddedSize * extent.height;

        std::string outDir = moduleDir() + "screenshots\\";
        CreateDirectoryA(outDir.c_str(), nullptr); // succeeds harmlessly if it already exists
        std::string outPath = outDir + timestampedFilename();

        std::ofstream file(outPath, std::ios::binary);
        if (!file)
        {
            Logger::warn("screenshot: could not open " + outPath + " for writing");
        }
        else
        {
            file.write(reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));
            file.write(reinterpret_cast<const char*>(&infoHeader), sizeof(infoHeader));

            // BMP rows are stored bottom-up; source data is top-down, R-G-B-A.
            // BMP wants B-G-R, no alpha.
            for (int32_t y = static_cast<int32_t>(extent.height) - 1; y >= 0; y--)
            {
                const uint8_t* srcRow = src + static_cast<size_t>(y) * extent.width * 4;
                for (uint32_t x = 0; x < extent.width; x++)
                {
                    row[x * 3 + 0] = srcRow[x * 4 + 2]; // B
                    row[x * 3 + 1] = srcRow[x * 4 + 1]; // G
                    row[x * 3 + 2] = srcRow[x * 4 + 0]; // R
                }
                file.write(reinterpret_cast<const char*>(row.data()), rowPaddedSize);
            }
            file.close();
            Logger::info("screenshot: saved " + outPath);
        }

        pLogicalDevice->vkd.UnmapMemory(pLogicalDevice->device, stagingMemory);
        pLogicalDevice->vkd.FreeMemory(pLogicalDevice->device, stagingMemory, nullptr);
        pLogicalDevice->vkd.DestroyBuffer(pLogicalDevice->device, stagingBuffer, nullptr);
    }

} // namespace vkbChoom
