// Назначение: базовый модуль ядра — логирование (в консоль и в файл) и общие утилиты.
#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace VfCore {

namespace fs = std::filesystem;

enum class Level { Info, Warn, Error };

class LogSink {
public:
    explicit LogSink(const fs::path& logPath = "vortex_forge.log") {
        stream_.open(logPath, std::ios::app);
    }

    ~LogSink() {
        if (stream_.is_open()) {
            stream_.close();
        }
    }

    void write(Level lvl, const std::string& msg) {
        std::string line = "[" + nowStr() + "] [" + lvlTag(lvl) + "] " + msg;
        entries_.push_back(line);

        if (lvl == Level::Error) {
            std::cerr << line << "\n";
        } else {
            std::cout << line << "\n";
        }

        if (stream_.is_open()) {
            stream_ << line << "\n";
            stream_.flush();
        }
    }

    const std::vector<std::string>& entries() const { return entries_; }

private:
    std::ofstream stream_;
    std::vector<std::string> entries_;

    static const char* lvlTag(Level lvl) {
        switch (lvl) {
            case Level::Info:  return "INFO";
            case Level::Warn:  return "WARN";
            case Level::Error: return "ERROR";
        }
        return "LOG";
    }

    static std::string nowStr() {
        auto tp = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(tp);
        std::tm tmBuf{};
#if defined(_WIN32)
        localtime_s(&tmBuf, &tt);
#else
        localtime_r(&tt, &tmBuf);
#endif
        std::ostringstream oss;
        oss << std::put_time(&tmBuf, "%Y-%m-%d %H:%M:%S");
        return oss.str();
    }
};

inline std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

}