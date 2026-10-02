#include "logger.hh"

#include <cstdlib>
#include <filesystem>
#include <chrono>
#include <ctime>
#include <iomanip>

#include <sstream>

namespace VKIntox
{
    Logger Logger::s_instance;

    Logger::Logger() : minLevel(getMinLogLevel())
    {
        if (minLevel != LogLevel::None)
        {
            std::string filename = getFileName();
            if (filename == "stderr")
            {
                outStream = {&std::cerr, {false}};
            }
            else if (filename == "stdout")
            {
                outStream = {&std::cout, {false}};
            }
            else
            {
                std::error_code ec;
                std::filesystem::path path(filename);
                std::filesystem::create_directories(path.parent_path(), ec);

                ec.clear();
                auto fileSize = std::filesystem::file_size(path, ec);
                if (!ec && fileSize > 4 * 1024 * 1024)
                {
                    std::filesystem::path oldPath = path;
                    oldPath += ".1";
                    std::filesystem::remove(oldPath, ec);
                    ec.clear();
                    std::filesystem::rename(path, oldPath, ec);
                }

                auto file = std::make_unique<std::ofstream>(path, std::ios::out | std::ios::app);
                if (*file)
                    outStream = {file.release(), {true}};
                else
                    outStream = {&std::cerr, {false}};
            }
        }
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
        // Early-out before taking lock if nothing to do
        if (level < minLevel && !historyEnabled)
            return;

        std::lock_guard<std::mutex> lock(mutex);

        // Store in history only if enabled (to save memory when debug window is off)
        if (historyEnabled)
        {
            history.push_back({level, message});
            if (history.size() > MAX_HISTORY_SIZE)
                history.pop_front();
        }

        if (level >= minLevel)
        {
            static std::array<const char*, 5> s_prefixes = {
                {"VKIntox trace: ", "VKIntox debug: ", "VKIntox info:  ", "VKIntox warn:  ", "VKIntox err:   "}};

            const char* prefix = s_prefixes.at(static_cast<uint32_t>(level));

            const auto now = std::chrono::system_clock::now();
            const auto time = std::chrono::system_clock::to_time_t(now);
            const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
            std::tm localTime{};
            localtime_r(&time, &localTime);

            std::stringstream stream(message);
            std::string       line;

            while (std::getline(stream, line, '\n'))
            {
                *outStream << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S")
                             << '.' << std::setfill('0') << std::setw(3) << milliseconds
                             << ' ' << prefix << line << '\n';
            }
            outStream->flush();
        }
    }

    std::vector<LogEntry> Logger::getHistory()
    {
        std::lock_guard<std::mutex> lock(s_instance.mutex);
        return std::vector<LogEntry>(s_instance.history.begin(), s_instance.history.end());
    }

    void Logger::clearHistory()
    {
        std::lock_guard<std::mutex> lock(s_instance.mutex);
        s_instance.history.clear();
    }

    void Logger::setHistoryEnabled(bool enabled)
    {
        std::lock_guard<std::mutex> lock(s_instance.mutex);
        s_instance.historyEnabled = enabled;
        if (!enabled)
            s_instance.history.clear();  // Free memory when disabled
    }

    bool Logger::isHistoryEnabled()
    {
        std::lock_guard<std::mutex> lock(s_instance.mutex);
        return s_instance.historyEnabled;
    }

    const char* Logger::levelName(LogLevel level)
    {
        static std::array<const char*, 5> names = {{"TRACE", "DEBUG", "INFO", "WARN", "ERROR"}};
        uint32_t idx = static_cast<uint32_t>(level);
        if (idx < names.size())
            return names[idx];
        return "UNKNOWN";
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

        const char* envVar = getenv("VKINTOX_LOG_LEVEL");

        const std::string logLevelStr = envVar ? envVar : "";

        for (const auto& [name, level] : logLevels)
        {
            if (logLevelStr == name)
                return level;
        }

        return LogLevel::Info;
    }

    std::string Logger::getFileName()
    {
        const char* envVar = getenv("VKINTOX_LOG_FILE");

        std::string filename = envVar ? envVar : "";

        if (filename.empty())
        {
            const char* configHome = getenv("XDG_CONFIG_HOME");
            const char* home = getenv("HOME");
            if (configHome && *configHome)
                filename = std::string(configHome) + "/VKIntox/vkintox.log";
            else if (home && *home)
                filename = std::string(home) + "/.config/VKIntox/vkintox.log";
            else
                filename = "stderr";
        }

        return filename;
    }

} // namespace VKIntox
