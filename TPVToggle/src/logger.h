#ifndef LOGGER_H
#define LOGGER_H

#include <string>
#include <fstream>
#include <mutex>

enum LogLevel
{
    LOG_TRACE = 0,
    LOG_DEBUG = 1,
    LOG_INFO = 2,
    LOG_WARNING = 3,
    LOG_ERROR = 4
};

class Logger
{
public:
    static Logger &getInstance()
    {
        static Logger instance;
        return instance;
    }

    void setLogLevel(LogLevel level);
    void log(LogLevel level, const std::string &message);

private:
    Logger();
    ~Logger();
    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;

    std::string getTimestamp() const;
    std::string generateLogFilePath() const;

    std::ofstream log_file_stream;
    LogLevel current_log_level;
    std::mutex log_mutex;
};

#endif // LOGGER_H
