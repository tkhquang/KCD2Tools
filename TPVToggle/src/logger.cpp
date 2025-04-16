/**
 * @file logger.cpp
 * @brief Implementation of the singleton Logger class.
 *
 * Provides file-based logging capabilities with multiple severity levels,
 * automatic timestamping, and log file path determination relative to the
 * running module (DLL/ASI). Uses standard C++ features and WinAPI for
 * module path retrieval.
 */

#include "logger.h"    // Corresponding header file
#include "constants.h" // For MOD_NAME and getLogFilename()
#include <windows.h>   // For GetModuleHandleExA, GetModuleFileNameA, GetLastError
#include <filesystem>  // Requires C++17 for path manipulation
#include <chrono>      // For system_clock, time_t conversions
#include <iomanip>     // For put_time, setw, left
#include <iostream>    // For cerr (error fallback)
#include <stdexcept>   // For runtime_error

/**
 * @brief Private Logger constructor. Sets default level, generates path, opens file.
 * @details Attempts to create/truncate the log file in the same directory as
 *          the module (DLL/ASI). Logs initialization status to the file itself
 *          or to stderr upon failure.
 */
Logger::Logger() : current_log_level(LOG_INFO) // Default log level is INFO
{
    // Determine the desired full path for the log file.
    std::string log_file_path = generateLogFilePath();

    // Attempt to open the file for writing, overwriting existing content.
    log_file_stream.open(log_file_path, std::ios::trunc);

    if (!log_file_stream.is_open())
    {
        // Critical failure: Cannot open the log file. Output error to stderr.
        // Using Constants::MOD_NAME to identify the source module.
        std::cerr << "[" << Constants::MOD_NAME << " Logger ERROR] "
                  << "Failed to open log file for writing: "
                  << log_file_path << std::endl;
        // log_file_stream remains in a failed state. Calls to log() will likely
        // either do nothing or only fallback to cerr for LOG_ERROR messages.
    }
    else
    {
        // Log successful initialization into the log file itself.
        // Use the log() method to ensure consistent formatting.
        log(LOG_INFO, "Logger initialized. Log file: " + log_file_path);
    }
}

/**
 * @brief Logger destructor. Ensures log file is flushed and closed gracefully.
 * @details Writes a final "shutting down" message to the log before closing
 *          the file stream handle.
 */
Logger::~Logger()
{
    if (log_file_stream.is_open())
    {
        log(LOG_INFO, "Logger shutting down.");
        log_file_stream.flush(); // Ensure all buffered data is written to the file
        log_file_stream.close(); // Close the file handle
    }
}

/**
 * @brief Sets the minimum logging level. Messages with severity lower than this
 *        level will be ignored.
 * @param level The new minimum LogLevel (e.g., LOG_INFO, LOG_DEBUG).
 * @details Logs the change of level itself at LOG_INFO severity for auditability.
 */
void Logger::setLogLevel(LogLevel level)
{
    // Capture the string representation of the *old* log level for the message.
    std::string oldLevelStr = "UNKNOWN";
    switch (current_log_level)
    {
    case LOG_DEBUG:
        oldLevelStr = "DEBUG";
        break;
    case LOG_INFO:
        oldLevelStr = "INFO";
        break;
    case LOG_WARNING:
        oldLevelStr = "WARNING";
        break;
    case LOG_ERROR:
        oldLevelStr = "ERROR";
        break;
    }

    // Atomically update the current log level.
    current_log_level = level;

    // Determine the string representation of the *new* log level.
    std::string newLevelStr = "UNKNOWN";
    switch (current_log_level) // Use the updated level
    {
    case LOG_DEBUG:
        newLevelStr = "DEBUG";
        break;
    case LOG_INFO:
        newLevelStr = "INFO";
        break;
    case LOG_WARNING:
        newLevelStr = "WARNING";
        break;
    case LOG_ERROR:
        newLevelStr = "ERROR";
        break;
    }
    // Log the level change using the newly set threshold.
    // Logging at INFO level ensures this message is usually visible.
    log(LOG_INFO, "Log level changed from " + oldLevelStr + " to " + newLevelStr);
}

/**
 * @brief Checks if the current logging level allows LOG_DEBUG messages.
 * @return true if the current log level is LOG_DEBUG (most verbose), false otherwise.
 */
bool Logger::isDebugEnabled() const
{
    // Log levels are ordered numerically (DEBUG=0, INFO=1, etc.).
    // DEBUG messages are logged only if the current level is set to LOG_DEBUG.
    return current_log_level <= LOG_DEBUG;
}

/**
 * @brief Writes a formatted message to the log file if its severity meets the
 *        configured threshold (`current_log_level`).
 * @param level The LogLevel severity of the message being logged.
 * @param message The content string of the log message.
 * @details Formats messages as "[YYYY-MM-DD HH:MM:SS] [LEVEL  ] :: Message".
 *          Includes a fallback mechanism: if file stream writing is not possible,
 *          critical (LOG_ERROR) messages are written to standard error (`stderr`).
 */
void Logger::log(LogLevel level, const std::string &message)
{
    // 1. Check if the message's severity level is sufficient to be logged.
    if (level >= current_log_level)
    {
        // 2. Primarily attempt to write to the log file stream.
        //    Check if the stream is open and not in an error state.
        if (log_file_stream.is_open() && log_file_stream.good())
        {
            // Determine the string representation for the log level enum.
            std::string level_str = "UNKNOWN";
            switch (level)
            {
            case LOG_DEBUG:
                level_str = "DEBUG";
                break;
            case LOG_INFO:
                level_str = "INFO";
                break;
            case LOG_WARNING:
                level_str = "WARNING";
                break;
            case LOG_ERROR:
                level_str = "ERROR";
                break;
                // No default needed as enum should cover all valid levels.
            }

            // Construct the log entry with timestamp, level (padded), and message.
            // std::endl automatically inserts a newline and flushes the stream buffer.
            log_file_stream << "[" << getTimestamp() << "] "
                            << "[" << std::setw(7) << std::left << level_str << "] :: "
                            << message << std::endl;
        }
        // 3. Fallback: If file logging failed and the message is critical (ERROR).
        //    Use 'else if' to avoid duplicate logging if file is working.
        else if (level >= LOG_ERROR)
        {
            // Write to standard error stream (cerr).
            // Prepend indication that this is a fallback due to file error.
            std::cerr << "[LOG_FILE_ERR] [" << getTimestamp() << "] [ERROR] :: "
                      << message << std::endl;
        }
        // Note: Non-ERROR messages are simply dropped if file stream isn't working.
    }
}

/**
 * @brief Retrieves the current timestamp formatted as "YYYY-MM-DD HH:MM:SS".
 * @return std::string The formatted timestamp string, or "TIMESTAMP_ERR" if
 *         an error occurs during time conversion or formatting.
 * @details Uses the C++ standard chrono library for time retrieval and
 *          platform-appropriate functions (`localtime_s` or `localtime`)
 *          for converting `time_t` to a calendar time structure (`tm`).
 *          Includes exception handling for robustness.
 */
std::string Logger::getTimestamp() const
{
    try
    {
        // Get the current system time point.
        const auto now = std::chrono::system_clock::now();
        // Convert the time point to a C-style time_t object.
        const auto in_time_t = std::chrono::system_clock::to_time_t(now);

        // Use a safe method to convert time_t to a broken-down 'tm' struct.
        std::tm timeinfo = {}; // IMPORTANT: Zero-initialize the struct first.

#if defined(_MSC_VER)
        // Use Microsoft's thread-safe version if compiling with MSVC.
        if (localtime_s(&timeinfo, &in_time_t) != 0)
        {
            // localtime_s returns non-zero on error.
            throw std::runtime_error("localtime_s failed to convert time");
        }
#else
        // Use standard C library's localtime for MinGW/GCC/others.
        // Note: Standard localtime uses a static internal buffer and is not
        // inherently thread-safe if called concurrently from multiple threads.
        // However, for typical mod usage where logging might be frequent but
        // less likely from intensely parallel sections calling *this exact
        // function*, it's often sufficient. Consider alternatives like
        // `localtime_r` if strict thread safety across the application's
        // use of time functions is required.
        std::tm *timeinfo_ptr = std::localtime(&in_time_t);
        if (!timeinfo_ptr)
        {
            // Check if localtime failed (returned null pointer).
            throw std::runtime_error("std::localtime returned null pointer");
        }
        // If successful, copy the data from the static buffer.
        timeinfo = *timeinfo_ptr;
#endif
        // Format the 'tm' struct into the desired "YYYY-MM-DD HH:MM:SS" string.
        std::ostringstream oss;
        oss << std::put_time(&timeinfo, "%Y-%m-%d %H:%M:%S");
        return oss.str();
    }
    catch (const std::exception &e)
    {
        // Log any standard exception during time processing to stderr.
        std::cerr << "[" << Constants::MOD_NAME << " Logger ERROR] Timestamp generation failed: "
                  << e.what() << std::endl;
        return "TIMESTAMP_ERR"; // Return an error indicator string.
    }
    catch (...)
    {
        // Catch any other potential C++ exceptions.
        std::cerr << "[" << Constants::MOD_NAME << " Logger ERROR] Unknown exception during timestamp generation." << std::endl;
        return "TIMESTAMP_ERR"; // Return an error indicator string.
    }
}

/**
 * @brief Determines the intended full path for the log file.
 * @details It attempts to place the log file in the same directory as the
 *          currently executing module (this DLL/ASI). It uses the WinAPI
 *          (`GetModuleHandleExA`, `GetModuleFileNameA`) to retrieve the module's path.
 *          If this process fails (e.g., API errors), it gracefully falls back
 *          to using just the base filename defined in `Constants`, which might
 *          result in the log file being created in the game's working directory.
 * @return std::string The calculated full path for the log file, or the base
 *         filename upon failure to determine the module path.
 */
std::string Logger::generateLogFilePath() const
{
    // Get the base log filename from Constants (e.g., "MyMod.log").
    const std::string base_filename = Constants::getLogFilename();
    // Initialize result path with the base filename as a fallback.
    std::string result_path = base_filename;

    HMODULE h_self = NULL;                   // Handle for the current module.
    char module_path_buffer[MAX_PATH] = {0}; // Buffer to store the full module path.

    try
    {
        // Get a handle to the code module that contains the `Logger::getInstance` function.
        // This reliably targets the current DLL/ASI.
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCSTR)&Logger::getInstance,
                                &h_self) ||
            h_self == NULL) // Explicitly check handle for NULL
        {
            // Throw an error if we couldn't get the module handle.
            throw std::runtime_error("GetModuleHandleExA failed (Error: " +
                                     std::to_string(GetLastError()) + ")");
        }

        // Retrieve the full path of the module identified by the handle.
        DWORD path_len = GetModuleFileNameA(h_self, module_path_buffer, MAX_PATH);

        // Check for errors reported by GetModuleFileNameA.
        if (path_len == 0)
        {
            // path_len is 0 on failure.
            throw std::runtime_error("GetModuleFileNameA failed (returned 0, Error: " +
                                     std::to_string(GetLastError()) + ")");
        }
        // Check if the buffer was too small (though MAX_PATH is usually sufficient).
        if (path_len == MAX_PATH && GetLastError() == ERROR_INSUFFICIENT_BUFFER)
        {
            throw std::runtime_error("GetModuleFileNameA failed (buffer too small)");
        }
        // Note: If path_len == MAX_PATH but no error, the path fit exactly,
        // but might be truncated if null termination was required. This is rare.

        // Use C++17 std::filesystem for robust path manipulation.
        std::filesystem::path module_full_path(module_path_buffer);
        // Combine the directory part of the module path with the base log filename.
        std::filesystem::path log_file_full_path = module_full_path.parent_path() / base_filename;

        // Convert the resulting filesystem path back to a string.
        result_path = log_file_full_path.string();
    }
    catch (const std::exception &e)
    {
        // If any standard exception occurred during path finding, log a warning
        // to stderr and proceed with the fallback path (base filename).
        std::cerr << "[" << Constants::MOD_NAME << " Logger WARNING] Failed to determine module directory: "
                  << e.what() << ". Using fallback log path: " << result_path << std::endl;
    }
    catch (...)
    {
        // Catch any other potential C++ exceptions.
        std::cerr << "[" << Constants::MOD_NAME << " Logger WARNING] Unknown exception during module path determination."
                  << " Using fallback log path: " << result_path << std::endl;
    }

    // Return the determined full path or the fallback base filename.
    return result_path;
}
