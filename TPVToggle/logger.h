#ifndef LOGGER_H
#define LOGGER_H

#include <string>
#include <fstream>

enum LogLevel
{
    LOG_DEBUG,   // Detailed diagnostic information for debugging
    LOG_INFO,    // General operational information
    LOG_WARNING, // Indications of potential issues
    LOG_ERROR    // Critical failures requiring attention
};

class Logger
{
public:
    // Returns the singleton instance of the Logger
    static Logger &getInstance();

    // Sets the minimum log level; messages below this level are ignored
    void setLogLevel(LogLevel level);

    // Logs a message with the specified level if it meets or exceeds the current level
    void log(LogLevel level, const std::string &message);

private:
    Logger();  // Private constructor to enforce singleton pattern
    ~Logger(); // Destructor to close the log file

    // Returns the current timestamp in "YYYY-MM-DD HH:MM:SS" format
    std::string getTimestamp() const;

    // Returns the log file name based on the current DLL's name (e.g., "KCD2_TPVToggle.log")
    std::string getLogFileName() const;

    std::ofstream log_file; // Output stream to the dynamically named log file
    LogLevel current_level; // Minimum level for logging
};

#endif // LOGGER_H
