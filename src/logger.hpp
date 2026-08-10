#ifndef LOGGER_HPP_INCLUDED
#define LOGGER_HPP_INCLUDED

#include <array>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <memory>
#include <functional>
#include <vector>
#include <cstdint>

namespace vkbChoom
{

    enum class LogLevel : uint32_t
    {
        Trace = 0,
        Debug = 1,
        Info  = 2,
        Warn  = 3,
        Error = 4,
        None  = 5,
    };

    class Logger
    {

    public:
        Logger();
        ~Logger();

        static void trace(const std::string& message);
        static void debug(const std::string& message);
        static void info(const std::string& message);
        static void warn(const std::string& message);
        static void err(const std::string& message);
        static void log(LogLevel level, const std::string& message);

        static LogLevel logLevel()
        {
            return s_instance.m_minLevel;
        }

        // Where the log actually ended up. Empty means "nowhere on disk,
        // OutputDebugString only". Worth surfacing in your own messages so
        // that the log can tell you where it is.
        static const std::string& resolvedPath()
        {
            return s_instance.m_resolvedPath;
        }

    private:
        using StreamPtr = std::unique_ptr<std::ostream, std::function<void(std::ostream*)>>;

        static Logger s_instance;

        const LogLevel m_minLevel;

        std::mutex m_mutex;

        StreamPtr   m_outStream;
        std::string m_resolvedPath;

        void emitMsg(LogLevel level, const std::string& message);

        static LogLevel getMinLogLevel();

        // Ordered list of absolute candidates. First one that opens wins.
        static std::vector<std::string> resolveLogPaths();
    };

} // namespace vkbChoom

#endif // LOGGER_HPP_INCLUDED
