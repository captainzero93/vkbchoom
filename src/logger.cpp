#include "logger.hpp"
#include "platform_win32.hpp"

// Stamped into the first line of every log so you can tell at a glance which
// DLL is actually loaded. If this line does not match the build you think you
// copied over, the game folder still has an old smaa_layer.dll in it.
#define VKBCHOOM_BUILD_TAG "RELEASE 1.10  (screenshots reverted to BMP -- PNG produced invalid output)"

#include <cstdlib>
#include <sstream>
#include <vector>
#include <cerrno>
#include <cstring>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace vkbChoom
{
    // ------------------------------------------------------------------
    // The old constructor did this:
    //
    //     m_outStream = ... new std::ofstream(filename) ...
    //
    // with filename defaulting to the RELATIVE "smaa_layer.log", and never
    // called is_open(). Two consequences, both of which made this bug
    // undebuggable:
    //
    //   1. The file landed in the process working directory, which for a
    //      Steam launch is normally the game folder but is NOT guaranteed
    //      -- a wrapper .bat, a shortcut with a different "Start in", or
    //      Steam's own launcher behaviour can put it elsewhere. You would
    //      then be looking for the log in the right place and it would be
    //      sitting somewhere else entirely.
    //
    //   2. If the open failed (read-only dir, Program Files without
    //      elevation, antivirus lock), the unique_ptr was still non-null
    //      and pointing at a failed stream. Every subsequent
    //      "*m_outStream << ..." set failbit and threw the message away in
    //      total silence. Loaded-and-broken and never-loaded produced a
    //      byte-identical result: no file.
    //
    // Now: absolute paths, tried in order, is_open() checked at each step,
    // and everything mirrored to OutputDebugStringA so there is always at
    // least one channel that works.
    // ------------------------------------------------------------------

    Logger::Logger() : m_minLevel(getMinLogLevel())
    {
        // Proof of life that does not depend on anything below succeeding.
        breadcrumb("Logger ctor reached (static init ran, CRT is alive)");

        if (m_minLevel == LogLevel::None)
        {
            breadcrumb("VKBCHOOM_LOG_LEVEL=none -- file logging disabled by request");
            return;
        }

        const std::vector<std::string> candidates = resolveLogPaths();

        for (const auto& path : candidates)
        {
            if (path == "stderr")
            {
                m_outStream = StreamPtr(&std::cerr, [](std::ostream*) {});
                m_resolvedPath = "stderr";
                breadcrumb("log -> stderr");
                return;
            }
            if (path == "stdout")
            {
                m_outStream = StreamPtr(&std::cout, [](std::ostream*) {});
                m_resolvedPath = "stdout";
                breadcrumb("log -> stdout");
                return;
            }

            auto file = new std::ofstream(path, std::ios::out | std::ios::trunc);
            if (file->is_open())
            {
                m_outStream    = StreamPtr(file, [](std::ostream* os) { delete os; });
                m_resolvedPath = path;
                breadcrumb("log -> " + path);
                *m_outStream << "vkbChoom info:  ===============================================" << std::endl;
                *m_outStream << "vkbChoom info:  BUILD = " << VKBCHOOM_BUILD_TAG << std::endl;
                *m_outStream << "vkbChoom info:  ===============================================" << std::endl;
                *m_outStream << "vkbChoom info:  log opened at " << path << std::endl;
                *m_outStream << "vkbChoom info:  dll  = " << modulePath() << std::endl;
                *m_outStream << "vkbChoom info:  exe  = " << exeDir() << exeName() << std::endl;
                *m_outStream << "vkbChoom info:  cwd  = " << currentDir() << std::endl;
                return;
            }

            delete file;
            breadcrumb("log open FAILED for " + path + " (errno=" + std::to_string(errno) + " "
                       + std::strerror(errno) + ")");
        }

        // Nothing on disk worked. Do NOT leave a dead stream in place --
        // fall through to the debugger channel only, and say so.
        m_outStream.reset();
        breadcrumb("no writable log path found -- OutputDebugString only from here on");
    }

    Logger::~Logger()
    {
    }

    void Logger::trace(const std::string& message)
    {
        s_instance.emitMsg(LogLevel::Trace, message);
    }

    void Logger::debug(const std::string& message)
    {
        s_instance.emitMsg(LogLevel::Debug, message);
    }

    void Logger::info(const std::string& message)
    {
        s_instance.emitMsg(LogLevel::Info, message);
    }

    void Logger::warn(const std::string& message)
    {
        s_instance.emitMsg(LogLevel::Warn, message);
    }

    void Logger::err(const std::string& message)
    {
        s_instance.emitMsg(LogLevel::Error, message);
    }

    void Logger::log(LogLevel level, const std::string& message)
    {
        s_instance.emitMsg(level, message);
    }

    void Logger::emitMsg(LogLevel level, const std::string& message)
    {
        if (level < m_minLevel)
            return;

        std::lock_guard<std::mutex> lock(m_mutex);

        static std::array<const char*, 5> s_prefixes = {
            {"vkbChoom trace: ", "vkbChoom debug: ", "vkbChoom info:  ", "vkbChoom warn:  ", "vkbChoom err:   "}};

        const char* prefix = s_prefixes.at(static_cast<uint32_t>(level));

        std::stringstream stream(message);
        std::string       line;

        while (std::getline(stream, line, '\n'))
        {
            if (m_outStream)
            {
                *m_outStream << prefix << line << std::endl;

                // If the stream goes bad mid-session (disk full, handle
                // yanked), stop pretending it worked.
                if (!m_outStream->good())
                {
                    m_outStream.reset();
                    OutputDebugStringA("[vkbchoom] log stream went bad; falling back to OutputDebugString\n");
                }
            }

            // Warnings and errors always go to the debugger channel too, so
            // that a failure is visible in DebugView even during a normal
            // healthy run where you are not watching the file.
            if (!m_outStream || level >= LogLevel::Warn)
            {
                OutputDebugStringA((std::string("[vkbchoom] ") + prefix + line + "\n").c_str());
            }
        }
    }

    LogLevel Logger::getMinLogLevel()
    {
        const std::array<std::pair<const char*, LogLevel>, 6> logLevels = {{
            {"trace", LogLevel::Trace},
            {"debug", LogLevel::Debug},
            {"info", LogLevel::Info},
            {"warn", LogLevel::Warn},
            {"error", LogLevel::Error},
            {"none", LogLevel::None},
        }};

        const char* envVar = getenv("VKBCHOOM_LOG_LEVEL");

        const std::string logLevelStr = envVar ? envVar : "";

        for (const auto& pair : logLevels)
        {
            if (logLevelStr == pair.first)
                return pair.second;
        }

        // Default is info, which yields roughly thirty lines per session: the
        // resolved paths, the config values actually loaded, every swapchain
        // creation with its VkResult, pipeline creation, and each F9 toggle.
        // That is enough to diagnose almost anything without the volume.
        //
        // Set VKBCHOOM_LOG_LEVEL=trace for the per-call function entry markers
        // (CreateInstance / CreateDevice / GetInstanceProcAddr queries). That
        // runs to several thousand lines, mostly from MGSV's repeated DXGI
        // adapter enumeration at startup, so only turn it on when chasing a
        // load-order or discovery problem.
        return LogLevel::Info;
    }

    std::vector<std::string> Logger::resolveLogPaths()
    {
        std::vector<std::string> paths;

        // 1. Explicit override always wins, exactly as given.
        if (const char* envVar = getenv("VKBCHOOM_LOG_FILE"))
        {
            std::string v(envVar);
            if (!v.empty())
                paths.push_back(v);
        }

        // 2. Next to smaa_layer.dll. This is the useful default: the DLL
        //    lives in the game folder next to vkbchoom.json, so the log
        //    ends up exactly where you are already looking, and it is
        //    independent of whatever the working directory happens to be.
        if (std::string dir = moduleDir(); !dir.empty())
            paths.push_back(dir + "smaa_layer.log");

        // 3. Next to the game exe (same place under a normal install, but
        //    differs if the DLL was installed to a shared location).
        if (std::string dir = exeDir(); !dir.empty())
            paths.push_back(dir + "smaa_layer.log");

        // 4. %LOCALAPPDATA%\vkbchoom\. Guaranteed writable, no elevation
        //    needed, survives the game folder being read-only.
        paths.push_back(fallbackLogDir() + "smaa_layer.log");

        return paths;
    }

} // namespace vkbChoom
