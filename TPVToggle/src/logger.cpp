#include "logger.h"
#include "constants.h"
#include <windows.h>
#include <iostream>
#include <iomanip>
#include <ctime>

Logger &Logger::getInstance()
{
    static Logger instance;
    return instance;
}

Logger::Logger() : current_level(LOG_DEBUG)
{
    std::string log_file_name = getLogFileName();
    log_file.open(log_file_name, std::ios::trunc);
    if (!log_file.is_open())
    {
        std::cerr << "Failed to open " << log_file_name << std::endl;
    }
}

Logger::~Logger()
{
    if (log_file.is_open())
    {
        log_file.close();
    }
}

void Logger::setLogLevel(LogLevel level)
{
    current_level = level;
}

std::string Logger::getTimestamp() const
{
    std::time_t now = std::time(nullptr);
    std::tm *local_time = std::localtime(&now);
    std::ostringstream oss;
    oss << std::put_time(local_time, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

std::string Logger::getLogFileName() const
{
    // Buffer to hold the DLL's full path
    char buffer[MAX_PATH];

    // Get the handle of the current module (this DLL)
    HMODULE hModule = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&Logger::getInstance, &hModule);

    // Retrieve the full path of the DLL
    DWORD length = GetModuleFileNameA(hModule, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
    {
        return Constants::getLogFilename(); // Fallback in case of failure
    }

    std::string full_path(buffer);

    // Extract the base name (e.g., "KCD2_TPVToggle" from "C:\path\KCD2_TPVToggle.asi")
    size_t last_slash = full_path.find_last_of("\\/");
    std::string file_name = (last_slash == std::string::npos) ? full_path : full_path.substr(last_slash + 1);
    size_t dot_pos = file_name.rfind(".asi");
    if (dot_pos != std::string::npos)
    {
        file_name = file_name.substr(0, dot_pos);
    }

    // Append .log extension
    return file_name + Constants::LOG_FILE_EXTENSION;
}

void Logger::log(LogLevel level, const std::string &message)
{
    if (level >= current_level && log_file.is_open())
    {
        std::string level_str;
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
        }
        log_file << "[" << getTimestamp() << "] [" << level_str << "] :: " << message << std::endl;
    }
}
