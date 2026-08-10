#ifndef SCREENSHOT_HPP_INCLUDED
#define SCREENSHOT_HPP_INCLUDED

#include "vulkan_include.hpp"
#include "logical_device.hpp"
#include "config.hpp"

namespace vkbChoom
{
    // Polls the configured screenshot key (default F11, see "screenshotKey"
    // in vkbchoom.conf) via GetAsyncKeyState -- a read-only query of current
    // system key state, not a hook -- so this cannot intercept, consume, or
    // delay the keypress in any way. The game (and anything else watching
    // that key) sees the exact same input it always would; this just
    // additionally notices it. Returns true on the single call where the
    // key transitions from up to down, not for every call while it's held.
    bool wasScreenshotKeyJustPressed(Config* pConfig);

    // Captures the given (real, post-effect) swapchain image to a BMP file
    // in <smaa_layer.dll's folder>\screenshots\. This is the same image
    // about to be handed to the real vkQueuePresentKHR -- i.e. genuinely
    // what's on screen, sharpening and all, unlike external capture tools
    // that may end up reading from an earlier point in the chain.
    //
    // Only R8G8B8A8_UNORM/_SRGB are handled (logs a warning and skips
    // otherwise, rather than writing a corrupted file) -- this is what
    // MGSV's swapchain has consistently used, but isn't guaranteed on every
    // system.
    //
    // Blocking: waits for the GPU to finish before writing the file. This
    // is deliberate and only costs anything on the frame a screenshot is
    // actually taken -- every other frame is completely unaffected.
    void captureScreenshot(LogicalDevice* pLogicalDevice, VkImage image, VkFormat format, VkExtent2D extent);

} // namespace vkbChoom

#endif // SCREENSHOT_HPP_INCLUDED
