/**
 * @file logger.h
 * @brief Defines a singleton Logger class for file-based logging.
 *
 * Provides log levels, timestamping, and automatic log file naming/placement
 * relative to the DLL. Uses C++17 filesystem for path handling.
 */
#ifndef LOGGER_H
#define LOGGER_H

#include <string>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <windows.h>  // For WinAPI path/module functions
#include <filesystem> // Requires C++17
#include <iostream>
#include <chrono>
#include "constants.h" // For fallback filename constants

/**
 * @enum LogLevel
 * @brief Severity levels for log messages (DEBUG=most verbose).
 */
enum LogLevel
{
    LOG_DEBUG,   /**< Detailed diagnostic info. */
    LOG_INFO,    /**< General operational info. */
    LOG_WARNING, /**< Potential issues. */
    LOG_ERROR    /**< Critical failures. */
};

/**
 * @class Logger
 * @brief Singleton for file logging with levels and timestamps.
 *
 * Creates log file (e.g., "MyMod.log") in DLL directory. Thread-safe access
 * via getInstance(). Falls back to stderr if file logging fails.
 */
class Logger
{
public:
    // --- Singleton Access ---
    /**
     * @brief Gets the singleton Logger instance (thread-safe C++11+).
     * @return Logger& Reference to the logger.
     */
    static Logger &getInstance()
    {
        static Logger instance;
        return instance;
    }

    // --- Configuration ---
    /**
     * @brief Sets the minimum severity level for messages to be logged.
     * @param level Minimum LogLevel to record (e.g., LOG_INFO).
     */
    void setLogLevel(LogLevel level);

    /**
     * @brief Checks if the current log level is DEBUG or lower (more verbose).
     * @return True if current level allows DEBUG messages, False otherwise.
     */
    bool isDebugEnabled() const;

    // --- Logging ---
    /**
     * @brief Writes a message if its level meets the current threshold.
     * @details Formats as "[Timestamp] [LEVEL  ] :: Message". Falls back to
     * stderr for ERROR level if file writing fails.
     * @param level The LogLevel severity of the message.
     * @param message The message content string.
     */
    void log(LogLevel level, const std::string &message);

    // --- Deleted constructors/assignment ---
    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;
    Logger(Logger &&) = delete;
    Logger &operator=(Logger &&) = delete;

private:
    Logger();
    ~Logger();
    std::string getTimestamp() const;
    std::string generateLogFilePath() const;

    std::ofstream log_file_stream;
    LogLevel current_log_level;
};

#endif // LOGGER_H
