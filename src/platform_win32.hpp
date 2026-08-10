#ifndef PLATFORM_WIN32_HPP_INCLUDED
#define PLATFORM_WIN32_HPP_INCLUDED

#include <string>

namespace vkbChoom
{
    // Writes an unconditional load-time record to
    // %LOCALAPPDATA%\vkbchoom\vkbchoom_load.log AND to OutputDebugStringA.
    // Uses only kernel32 -- safe from DllMain, safe before/independent of
    // CRT and iostream state, and immune to the working directory.
    void breadcrumb(const std::string& message);

    std::string modulePath();     // full path to smaa_layer.dll
    std::string moduleDir();      // directory of smaa_layer.dll, trailing '\'
    std::string exeDir();         // directory of the host .exe, trailing '\'
    std::string exeName();        // e.g. "mgsvtpp.exe"
    std::string currentDir();     // process CWD, trailing '\'
    std::string fallbackLogDir(); // %LOCALAPPDATA%\vkbchoom\, created if absent

} // namespace vkbChoom

#endif // PLATFORM_WIN32_HPP_INCLUDED
