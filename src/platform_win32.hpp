#ifndef PLATFORM_WIN32_HPP_INCLUDED
#define PLATFORM_WIN32_HPP_INCLUDED

#include <string>

namespace vkbChoom
{
    // THE PROCESS GATE.
    //
    // vkbchoom.json registers as a Vulkan GLOBAL implicit layer, because
    // that is the only registration the Vulkan loader has -- there is no
    // "load this layer for one executable" concept. So smaa_layer.dll gets
    // LoadLibrary'd into EVERY process on this machine that creates a Vulkan
    // instance: emulators, other games, vulkaninfo, GPU control panels.
    //
    // layerShouldRunHere() is the one decision that keeps that from being a
    // problem. It is deliberately built to be answerable at the earliest
    // possible moment, before anything else in this DLL has initialised:
    //
    //   * it depends on nothing but kernel32 -- no Config object, no
    //     iostreams, no Logger, no heap beyond std::string, nothing that can
    //     throw or that needs static init to have completed
    //   * it caches its answer in a plain int with a constant initialiser,
    //     so there is no magic-static guard to take under the loader lock
    //   * it is safe to call from DllMain and from
    //     vkNegotiateLoaderLayerInterfaceVersion, which are the two earliest
    //     points the loader can reach us
    //
    // Resolution order for the target executable name:
    //   1. %VKBCHOOM_TARGET_EXE%          (';'-separated list, for testing)
    //   2. targetExecutable = ... in vkbchoom.conf, next to the DLL, then
    //      next to the host exe -- scanned with raw CreateFile/ReadFile
    //   3. "mgsvtpp.exe"
    bool        layerShouldRunHere();
    std::string targetExecutableName(); // for diagnostics/logging only

    // Writes a load-time record to %LOCALAPPDATA%\vkbchoom\vkbchoom_load.log
    // AND to OutputDebugStringA. Uses only kernel32 -- safe from DllMain,
    // safe before/independent of CRT and iostream state, and immune to the
    // working directory.
    //
    // GATED: in a process that is not the target this writes nothing at all,
    // not even the debugger line. Creating files and spamming
    // OutputDebugString from inside somebody else's emulator is exactly the
    // kind of side effect a layer that is supposed to be inert must not
    // have. Set VKBCHOOM_FORCE_LOG=1 to log everywhere anyway when you are
    // deliberately debugging the gate itself.
    void breadcrumb(const std::string& message);

    std::string modulePath();     // full path to smaa_layer.dll
    std::string moduleDir();      // directory of smaa_layer.dll, trailing '\'
    std::string exeDir();         // directory of the host .exe, trailing '\'
    std::string exeName();        // e.g. "mgsvtpp.exe"
    std::string currentDir();     // process CWD, trailing '\'
    std::string fallbackLogDir(); // %LOCALAPPDATA%\vkbchoom\, created if absent

} // namespace vkbChoom

#endif // PLATFORM_WIN32_HPP_INCLUDED
