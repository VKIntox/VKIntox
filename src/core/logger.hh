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
#include <deque>

namespace VKIntox
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

    // Log entry for history
    struct LogEntry
    {
        LogLevel level;
        std::string message;
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

        // Cheap level check for lazy logging macros. Reads the const min level
        // without taking the mutex. Use VKINTOX_LOG_DEBUG / VKINTOX_LOG_TRACE
        // macros (defined below) to avoid constructing the message string when
        // the level is filtered out.
        static bool isLevelEnabled(LogLevel level)
        {
            return level >= s_instance.m_minLevel || s_instance.m_historyEnabled;
        }

        // Get log history (thread-safe copy)
        static std::vector<LogEntry> getHistory();

        // Clear log history
        static void clearHistory();

        // Enable/disable history storage (disabled by default to save memory)
        static void setHistoryEnabled(bool enabled);
        static bool isHistoryEnabled();

        // Get level name string
        static const char* levelName(LogLevel level);

    private:
        static Logger s_instance;
        static constexpr size_t MAX_HISTORY_SIZE = 1000;

        const LogLevel m_minLevel;

        std::mutex m_mutex;

        std::unique_ptr<std::ostream, std::function<void(std::ostream*)>> m_outStream;

        std::deque<LogEntry> m_history;
        bool m_historyEnabled = false;  // Disabled by default to save memory

        void emitMsg(LogLevel level, const std::string& message);

        static LogLevel getMinLogLevel();

        static std::string getFileName();
    };

} // namespace VKIntox

// Lazy-evaluated logging macros. Use these on hot paths (per-frame, per-draw)
// to avoid paying for std::string concatenation when the corresponding log
// level is filtered out. The condition is checked once; the message stream
// is only built if the level is enabled.
//
//   VKINTOX_LOG_DEBUG([&](std::string& s) { s = "foo=" + std::to_string(x); });
//
#define VKINTOX_LOG_TRACE(msg_fn) \
    do { if (::VKIntox::Logger::isLevelEnabled(::VKIntox::LogLevel::Trace)) { \
        std::string _m; msg_fn(_m); ::VKIntox::Logger::trace(_m); } } while (false)
#define VKINTOX_LOG_DEBUG(msg_fn) \
    do { if (::VKIntox::Logger::isLevelEnabled(::VKIntox::LogLevel::Debug)) { \
        std::string _m; msg_fn(_m); ::VKIntox::Logger::debug(_m); } } while (false)
#define VKINTOX_LOG_INFO(msg_fn) \
    do { if (::VKIntox::Logger::isLevelEnabled(::VKIntox::LogLevel::Info)) { \
        std::string _m; msg_fn(_m); ::VKIntox::Logger::info(_m); } } while (false)
#define VKINTOX_LOG_WARN(msg_fn) \
    do { if (::VKIntox::Logger::isLevelEnabled(::VKIntox::LogLevel::Warn)) { \
        std::string _m; msg_fn(_m); ::VKIntox::Logger::warn(_m); } } while (false)

#endif // LOGGER_HPP_INCLUDED
