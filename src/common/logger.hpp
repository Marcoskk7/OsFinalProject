#pragma once

#include <iostream>
#include <string_view>
#include <thread>

namespace osp
{

enum class LogLevel
{
    Debug,
    Info,
    Warn,
    Error
};

inline void log(LogLevel level, std::string_view msg)
{
    const char* tag = "";
    switch (level)
    {
    case LogLevel::Debug: tag = "[DEBUG]"; break;
    case LogLevel::Info: tag = "[INFO ]"; break;
    case LogLevel::Warn: tag = "[WARN ]"; break;
    case LogLevel::Error: tag = "[ERROR]"; break;
    }

    std::clog << tag << " [tid=" << std::this_thread::get_id() << "] " << msg << '\n';
}

} // namespace osp


