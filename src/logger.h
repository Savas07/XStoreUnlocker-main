#pragma once

#include <string>
#include <fstream>
#include <mutex>
#include <cstdio>
#include <Windows.h>

class Logger {
public:
    static Logger& Instance() {
        static Logger inst;
        return inst;
    }

    void Init(const std::string& path) {
        std::lock_guard<std::mutex> lock(m_mtx);
        m_file.open(path, std::ios::out | std::ios::trunc);
    }

    void SetEnabled(bool on) { m_on = on; }

    void Info(const char* fmt, ...) {
        if (!m_on) return;
        va_list a; va_start(a, fmt);
        Write("INFO", fmt, a);
        va_end(a);
    }

    void Warn(const char* fmt, ...) {
        if (!m_on) return;
        va_list a; va_start(a, fmt);
        Write("WARN", fmt, a);
        va_end(a);
    }

    void Error(const char* fmt, ...) {
        if (!m_on) return;
        va_list a; va_start(a, fmt);
        Write("ERROR", fmt, a);
        va_end(a);
    }

    ~Logger() {
        if (m_file.is_open()) m_file.close();
    }

private:
    Logger() = default;

    void Write(const char* level, const char* fmt, va_list args) {
        std::lock_guard<std::mutex> lock(m_mtx);

        char msg[4096];
        vsnprintf(msg, sizeof(msg), fmt, args);

        char dbg[4200];
        snprintf(dbg, sizeof(dbg), "[XStoreUnlocker][%s] %s\n", level, msg);
        OutputDebugStringA(dbg);

        if (!m_file.is_open()) return;

        SYSTEMTIME t;
        GetLocalTime(&t);

        char ts[32];
        snprintf(ts, sizeof(ts), "%02d:%02d:%02d.%03d",
            t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);

        m_file << "[" << ts << "][" << level << "] " << msg << std::endl;
    }

    std::ofstream m_file;
    std::mutex m_mtx;
    bool m_on = false;
};

#define LOG_INFO(...)  Logger::Instance().Info(__VA_ARGS__)
#define LOG_WARN(...)  Logger::Instance().Warn(__VA_ARGS__)
#define LOG_ERROR(...) Logger::Instance().Error(__VA_ARGS__)
