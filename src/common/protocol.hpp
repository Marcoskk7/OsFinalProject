#pragma once

#include "types.hpp"

#include <string>

namespace osp::protocol
{

// 简化版协议：后续可以根据课程要求扩展为结构化二进制或 JSON 等
enum class MessageType
{
    AuthRequest,
    AuthResponse,
    CommandRequest,
    CommandResponse,
    Error
};

struct Message
{
    MessageType type{};
    std::string payload; // 暂时使用文本形式，后续可自行扩展
};

inline std::string serialize(const Message& msg)
{
    // 非正式实现：type 整数 + '\n' + payload
    return std::to_string(static_cast<int>(msg.type)) + '\n' + msg.payload;
}

inline Message deserialize(const std::string& data)
{
    auto pos = data.find('\n');
    Message msg{};
    if (pos == std::string::npos)
    {
        msg.type = MessageType::Error;
        msg.payload = "Malformed message";
        return msg;
    }

    int t = std::stoi(data.substr(0, pos));
    msg.type = static_cast<MessageType>(t);
    msg.payload = data.substr(pos + 1);
    return msg;
}

} // namespace osp::protocol


