#pragma once
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

class Logger {
public:
    static void Init(const std::string &filename) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_file.open(filename, std::ios::out | std::ios::trunc);
        if (m_file.is_open()) {
            m_file << "--- Logger Initialized ---" << std::endl;
        }
    }

    static void Log(const std::string &message) {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

        std::stringstream ss;
        struct tm timeinfo;
        localtime_s(&timeinfo, &time);
        ss << "[" << std::put_time(&timeinfo, "%Y-%m-%d %H:%M:%S") << "." << std::setfill('0') << std::setw(3)
           << ms.count() << "] " << message << std::endl;

        if (m_file.is_open()) {
            m_file << ss.str();
            m_file.flush();
        } else {
            std::cout << ss.str();
        }
    }

private:
    inline static std::ofstream m_file;
    inline static std::mutex m_mutex;
};

#define LOG(msg) Logger::Log(msg)
