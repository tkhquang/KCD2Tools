/**
 * @file logger.cpp
 * @brief Implementation of the singleton Logger class.
 */

#include "logger.h"
#include "constants.h"
#include <windows.h>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <stdexcept>

Logger::Logger() : current_log_level(LOG_INFO)
{
    std::string log_file_path = generateLogFilePath();
    log_file_stream.open(log_file_path, std::ios::trunc);
    if (!log_file_stream.is_open())
    {
        std::cerr << "[" << Constants::MOD_NAME << " Logger ERROR] "
                  << "Failed to open log file: " << log_file_path << std::endl;
    }
    else
    {
        log(LOG_INFO, "Logger initialized. Log file: " + log_file_path);
    }
}

Logger::~Logger()
{
    if (log_file_stream.is_open())
    {
        std::lock_guard<std::mutex> lock(log_mutex);
        log_file_stream << "[" << getTimestamp() << "] [INFO   ] :: Logger shutting down." << std::endl;
        log_file_stream.flush();
        log_file_stream.close();
    }
}

void Logger::setLogLevel(LogLevel level)
{
    std::string oldLevelStr = "UNKNOWN";
    switch (current_log_level)
    {
    case LOG_TRACE:
        oldLevelStr = "TRACE";
        break;
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
    current_log_level = level;
    std::string newLevelStr = "UNKNOWN";
    switch (current_log_level)
    {
    case LOG_TRACE:
        newLevelStr = "TRACE";
        break;
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
    log(LOG_INFO, "Log level changed from " + oldLevelStr + " to " + newLevelStr);
}

void Logger::log(LogLevel level, const std::string &message)
{
    if (level >= current_log_level)
    {
        std::string level_str = "UNKNOWN";
        switch (level)
        {
        case LOG_TRACE:
            level_str = "TRACE";
            break;
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
        }
        std::lock_guard<std::mutex> lock(log_mutex);
        if (log_file_stream.is_open() && log_file_stream.good())
        {
            log_file_stream << "[" << getTimestamp() << "] "
                            << "[" << std::setw(7) << std::left << level_str << "] :: "
                            << message << std::endl;
        }
        else if (level >= LOG_ERROR)
        {
            std::cerr << "[LOG_FILE_ERR] [" << getTimestamp() << "] ["
                      << std::setw(7) << std::left << level_str << "] :: "
                      << message << std::endl;
        }
    }
}

std::string Logger::getTimestamp() const
{
    try
    {
        const auto now = std::chrono::system_clock::now();
        const auto in_time_t = std::chrono::system_clock::to_time_t(now);
        std::tm timeinfo = {};
#if defined(_MSC_VER)
        if (localtime_s(&timeinfo, &in_time_t) != 0)
        {
            throw std::runtime_error("localtime_s failed");
        }
#else
        std::tm *timeinfo_ptr = std::localtime(&in_time_t);
        if (!timeinfo_ptr)
            throw std::runtime_error("std::localtime returned null");
        timeinfo = *timeinfo_ptr;
#endif
        std::ostringstream oss;
        oss << std::put_time(&timeinfo, "%Y-%m-%d %H:%M:%S");
        return oss.str();
    }
    catch (const std::exception &e)
    {
        std::cerr << "[" << Constants::MOD_NAME << " Logger ERROR] Timestamp failed: " << e.what() << std::endl;
        return "TIMESTAMP_ERR";
    }
    catch (...)
    {
        std::cerr << "[" << Constants::MOD_NAME << " Logger ERROR] Unknown timestamp exception." << std::endl;
        return "TIMESTAMP_ERR";
    }
}

std::string Logger::generateLogFilePath() const
{
    const std::string base_filename = Constants::getLogFilename();
    std::string result_path = base_filename;
    HMODULE h_self = NULL;
    char module_path_buffer[MAX_PATH] = {0};
    try
    {
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCSTR)&Logger::getInstance, &h_self) ||
            h_self == NULL)
        {
            throw std::runtime_error("GetModuleHandleExA failed: " + std::to_string(GetLastError()));
        }
        DWORD path_len = GetModuleFileNameA(h_self, module_path_buffer, MAX_PATH);
        if (path_len == 0)
            throw std::runtime_error("GetModuleFileNameA failed: " + std::to_string(GetLastError()));
        if (path_len == MAX_PATH && GetLastError() == ERROR_INSUFFICIENT_BUFFER)
            throw std::runtime_error("GetModuleFileNameA failed: Buffer too small");
        std::filesystem::path module_full_path(module_path_buffer);
        std::filesystem::path log_file_full_path = module_full_path.parent_path() / base_filename;
        result_path = log_file_full_path.string();
    }
    catch (const std::exception &e)
    {
        std::cerr << "[" << Constants::MOD_NAME << " Logger WARNING] Failed get module dir: " << e.what() << ". Using fallback: " << result_path << std::endl;
    }
    catch (...)
    {
        std::cerr << "[" << Constants::MOD_NAME << " Logger WARNING] Unknown exception get module path. Using fallback: " << result_path << std::endl;
    }
    return result_path;
}
