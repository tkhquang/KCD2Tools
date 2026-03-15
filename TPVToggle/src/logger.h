/**
 * @file logger.h
 * @brief Adapter header that provides compatibility layer over DetourModKit Logger.
 *
 * This header adapts the original TPVToggle Logger interface to use DMKLogger
 * internally, allowing minimal changes to existing code while migrating to
 * DetourModKit as the underlying framework.
 */

#ifndef LOGGER_H
#define LOGGER_H

#include <DetourModKit.hpp>
#include <string>

// Legacy LogLevel enum - maps directly to DMK::LogLevel
enum LogLevel
{
    LOG_TRACE = static_cast<int>(DMK::LogLevel::LOG_TRACE),
    LOG_DEBUG = static_cast<int>(DMK::LogLevel::LOG_DEBUG),
    LOG_INFO = static_cast<int>(DMK::LogLevel::LOG_INFO),
    LOG_WARNING = static_cast<int>(DMK::LogLevel::LOG_WARNING),
    LOG_ERROR = static_cast<int>(DMK::LogLevel::LOG_ERROR)
};

/**
 * @brief Logger adapter class that wraps DMKLogger.
 * @details Provides the original TPVToggle Logger interface while using
 *          DetourModKit's DMKLogger as the underlying implementation.
 */
class Logger
{
public:
    static Logger &getInstance()
    {
        static Logger instance;
        return instance;
    }

    /**
     * @brief Sets the minimum log level.
     * @param level The minimum level to log.
     */
    void setLogLevel(LogLevel level)
    {
        m_currentLogLevel = level;
        DMKLogger::getInstance().setLogLevel(static_cast<DMK::LogLevel>(level));
    }

    /**
     * @brief Gets the current log level.
     * @return LogLevel The current minimum log level.
     */
    LogLevel getLogLevel() const
    {
        return m_currentLogLevel;
    }

    /**
     * @brief Logs a message with the specified level.
     * @param level The severity level of the message.
     * @param message The message to log.
     */
    void log(LogLevel level, const std::string &message)
    {
        DMKLogger::getInstance().log(static_cast<DMK::LogLevel>(level), message);
    }

    /**
     * @brief Configures the logger settings (forwarded to DMKLogger).
     * @param prefix The log prefix (e.g., mod name).
     * @param file_name The log file name.
     * @param timestamp_fmt The timestamp format string.
     */
    static void configure(const std::string &prefix, const std::string &file_name, const std::string &timestamp_fmt)
    {
        DMKLogger::configure(prefix, file_name, timestamp_fmt);
    }

private:
    LogLevel m_currentLogLevel = LOG_INFO; // Track log level locally (DMKLogger doesn't expose getter)

    Logger() = default;
    ~Logger() = default;
    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;
};

#endif // LOGGER_H
