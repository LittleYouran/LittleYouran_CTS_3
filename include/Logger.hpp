#pragma once 

#include "Utils.hpp"
#include "LibUtils.hpp"

class Logger {
private:
    static constexpr const char* logPath = "/sdcard/Android/CTS/log.txt";

    constexpr static int LINE_SIZE = 1024 * 32;   //  32 KiB
    char lineCache[LINE_SIZE];

    LOG_LEVEL logLevel_ = LOG_LEVEL::INFO;
    mutex logPrintMutex;
public:

    void Debug(const string_view& message) {
        Log(LOG_LEVEL::DEBUG, message);
    } 

    void Info(const string_view& message) {
        Log(LOG_LEVEL::INFO, message);
    }

    void Warn(const string_view& message) {
        Log(LOG_LEVEL::WARN, message);
    }

    void Error(const string_view& message) {
        Log(LOG_LEVEL::ERROR, message);
    }

    template<typename... Args>
    void Debug(const string_view& message, Args&&... args) {
        Log(LOG_LEVEL::DEBUG, message, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void Info(const string_view& message, Args&&... args) {
        Log(LOG_LEVEL::INFO, message, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void Warn(const string_view& message, Args&&... args) {
        Log(LOG_LEVEL::WARN, message, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void Error(const string_view& message, Args&&... args) {
        Log(LOG_LEVEL::ERROR, message, std::forward<Args>(args)...);
    }

    void setLogLevel(const string_t& level) {
        if (level == "DEBUG") 
            logLevel_ = LOG_LEVEL::DEBUG;
        else if (level == "INFO") 
            logLevel_ = LOG_LEVEL::INFO;
        else if (level == "WARN") 
            logLevel_ = LOG_LEVEL::WARN;
        else if (level == "ERROR") 
            logLevel_ = LOG_LEVEL::ERROR;
    }

    void clear_log() {
        auto temp = fopen(logPath, "wb");
        if (!temp) {
            fprintf(stderr, "ERROR:清理日志文件失败");
            return;
        }
        fclose(temp);
    }

private:
    void WriteFile(const char* content, const int len) noexcept {
        int fd = open(logPath, O_WRONLY | O_APPEND | O_CREAT, 0666); 

        if (fd >= 0) {
            write(fd, content, len);
            close(fd);
        }
    }


    int getCurrentTimeStr(char* buf, size_t size) {
        time_t now = time(nullptr);
        struct tm* local_time = localtime(&now);
        return strftime(buf, size, "%Y-%m-%d %H:%M:%S ", local_time);
    }

    void Log(LOG_LEVEL level, const string_view& message) {
        if (level < logLevel_) return;

        lock_guard<mutex> lock(logPrintMutex);

        size_t len = getCurrentTimeStr(lineCache, sizeof(lineCache));

        const string_view levelStr = levelStrings[static_cast<size_t>(level)]; 
        memcpy(lineCache + len, levelStr.data(), levelStr.size());
        len += levelStr.size();

        lineCache[len++] = ' ';

        memcpy(lineCache + len, message.data(), message.length());
        len += message.length();

        lineCache[len++] = '\n';
        lineCache[len] = '\0';

        WriteFile(lineCache, len);
    }

    template<typename... Args>
    void Log(LOG_LEVEL level, const string_view& format, Args&&... args) {
        if (level < logLevel_) return;  
        
        lock_guard<mutex> lock(logPrintMutex);

        size_t len = getCurrentTimeStr(lineCache, sizeof(lineCache));
        const string_view levelStr = levelStrings[static_cast<size_t>(level)];

        memcpy(lineCache + len, levelStr.data(), levelStr.size());
        len += levelStr.size();
        lineCache[len++] = ' ';
        int fmtLen = FastSnprintf(lineCache + len, sizeof(lineCache) - len, format.data(), std::forward<Args>(args)...);

        len += static_cast<size_t>(fmtLen);
        lineCache[len++] = '\n';
        lineCache[len] = '\0';

        WriteFile(lineCache, len);
    }

    inline static constexpr string_view levelStrings[] = {
        "调试 ->",
        "信息 ->",
        "警告 ->",
        "错误 ->",
    };
}; 