#pragma once

#include "types.hpp"

#include <sstream>
#include <string>
#include <vector>

namespace osp::protocol
{

// --------------------- 基础消息封装 ---------------------

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

// 传输层序列化：type 整数 + '\n' + payload
inline std::string serialize(const Message& msg)
{
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

// --------------------- 统一命令协议层 ---------------------

// 统一的「命令」抽象：
// - name      : 命令名（例如 PING / MKDIR / LIST_PAPERS 等）
// - rawArgs   : 去掉命令名后，整行剩余的字符串（用于 WRITE 等需要保留空格的场景）
// - args      : 按空格分割 rawArgs 得到的参数数组，适合大多数简单命令使用
// - sessionId : 可选，会话 ID；当 payload 使用 "SESSION <id> CMD CMD_NAME ..." 形式时填充
struct Command
{
    std::string              name;
    std::string              rawArgs;
    std::vector<std::string> args;
    std::string              sessionId; // 为空表示未携带 Session
};

// 解析单纯的命令行（不含 SESSION/CMD 前缀）：如 "MKDIR /demo"。
inline Command parseCommandLine(const std::string& line)
{
    Command cmd;

    std::size_t start = line.find_first_not_of(' ');
    if (start == std::string::npos)
    {
        return cmd; // 空命令
    }

    // 提取命令名
    std::size_t spacePos = line.find(' ', start);
    if (spacePos == std::string::npos)
    {
        cmd.name = line.substr(start);
        cmd.rawArgs.clear();
        return cmd;
    }

    cmd.name = line.substr(start, spacePos - start);

    // 去掉命令名后面的连续空格，得到 rawArgs
    std::size_t argStart = line.find_first_not_of(' ', spacePos);
    if (argStart == std::string::npos)
    {
        cmd.rawArgs.clear();
        return cmd;
    }

    cmd.rawArgs = line.substr(argStart);

    // 按空格切分 rawArgs，得到 args（对不含空格的普通参数足够；需要保留空格时用 rawArgs）
    std::istringstream iss(cmd.rawArgs);
    std::string        token;
    while (iss >> token)
    {
        cmd.args.push_back(token);
    }

    return cmd;
}

// 从 Message.payload 解析出统一的 Command 结构。
// 支持两种形式：
// 1) 无会话： "CMD_NAME arg1 arg2 ..."
// 2) 携带会话： "SESSION <sessionId> CMD CMD_NAME arg1 arg2 ..."
inline Command parseCommandPayload(const std::string& payload)
{
    Command cmd;

    // 去掉开头空白
    std::size_t start = payload.find_first_not_of(' ');
    if (start == std::string::npos)
    {
        return cmd; // 空命令
    }

    constexpr const char* kSessionTag = "SESSION";
    constexpr const char* kCmdTag = "CMD";

    const auto hasSessionPrefix =
        payload.compare(start, std::char_traits<char>::length(kSessionTag), kSessionTag) == 0
        && (start + std::char_traits<char>::length(kSessionTag) >= payload.size()
            || payload[start + std::char_traits<char>::length(kSessionTag)] == ' ');

    if (!hasSessionPrefix)
    {
        // 兼容旧格式：整行就是一个命令
        return parseCommandLine(payload);
    }

    // 解析 "SESSION <sessionId> CMD <...>"
    std::size_t pos = start + std::char_traits<char>::length(kSessionTag);

    // 跳过空格
    pos = payload.find_first_not_of(' ', pos);
    if (pos == std::string::npos)
    {
        return cmd; // 只有 "SESSION" 没有内容，视为无效
    }

    // 读取 sessionId（直到下一个空格或行尾）
    const std::size_t sessionStart = pos;
    pos = payload.find(' ', pos);
    if (pos == std::string::npos)
    {
        // 只有 "SESSION <id>"，未指定 CMD 和具体命令
        return cmd;
    }

    cmd.sessionId = payload.substr(sessionStart, pos - sessionStart);
    if (cmd.sessionId.empty())
    {
        return Command{};
    }

    // 跳过空格，期望出现 "CMD"
    pos = payload.find_first_not_of(' ', pos);
    if (pos == std::string::npos)
    {
        return Command{};
    }

    const auto hasCmdTag =
        payload.compare(pos, std::char_traits<char>::length(kCmdTag), kCmdTag) == 0
        && (pos + std::char_traits<char>::length(kCmdTag) >= payload.size()
            || payload[pos + std::char_traits<char>::length(kCmdTag)] == ' ');

    if (!hasCmdTag)
    {
        // 不是期望的 "CMD"，视为格式错误
        return Command{};
    }

    // 跳过 "CMD" 及其后的空格，剩余部分为真正的命令行
    pos += std::char_traits<char>::length(kCmdTag);
    pos = payload.find_first_not_of(' ', pos);
    if (pos == std::string::npos)
    {
        return Command{}; // 有 CMD 但没有具体命令
    }

    Command inner = parseCommandLine(payload.substr(pos));
    inner.sessionId = cmd.sessionId;
    return inner;
}

// 便于客户端构造命令：根据 Command 生成 payload 字符串
inline std::string buildCommandPayload(const Command& cmd)
{
    std::ostringstream oss;

    if (!cmd.sessionId.empty())
    {
        oss << "SESSION " << cmd.sessionId << " CMD ";
    }

    oss << cmd.name;

    if (!cmd.rawArgs.empty())
    {
        oss << ' ' << cmd.rawArgs;
        return oss.str();
    }

    for (const auto& arg : cmd.args)
    {
        oss << ' ' << arg;
    }
    return oss.str();
}

} // namespace osp::protocol


