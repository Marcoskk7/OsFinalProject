#pragma once

#include <iostream>
#include <string_view>

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

    std::clog << tag << ' ' << msg << '\n';
}

} // namespace osp


