#pragma once

#include "types.hpp"
#include "third_party/json.hpp"

#include <sstream>
#include <string>
#include <vector>

namespace osp::protocol
{

using json = nlohmann::json;

// --------------------- 基础消息封装 ---------------------

enum class MessageType
{
    AuthRequest,
    AuthResponse,
    CommandRequest,
    CommandResponse,
    Error
};

// MessageType <-> string 转换
inline std::string messageTypeToString(MessageType t)
{
    switch (t)
    {
    case MessageType::AuthRequest: return "AuthRequest";
    case MessageType::AuthResponse: return "AuthResponse";
    case MessageType::CommandRequest: return "CommandRequest";
    case MessageType::CommandResponse: return "CommandResponse";
    case MessageType::Error: return "Error";
    }
    return "Unknown";
}

inline MessageType stringToMessageType(const std::string& s)
{
    if (s == "AuthRequest") return MessageType::AuthRequest;
    if (s == "AuthResponse") return MessageType::AuthResponse;
    if (s == "CommandRequest") return MessageType::CommandRequest;
    if (s == "CommandResponse") return MessageType::CommandResponse;
    return MessageType::Error;
}

// 消息结构：payload 改为 JSON
struct Message
{
    MessageType type{};
    json        payload; // JSON 格式的 payload
};

// 传输层序列化：整个 envelope 为 JSON { "type": "...", "payload": {...} }
inline std::string serialize(const Message& msg)
{
    json envelope;
    envelope["type"] = messageTypeToString(msg.type);
    envelope["payload"] = msg.payload;
    return envelope.dump();
}

inline Message deserialize(const std::string& data)
{
    Message msg{};
    try
    {
        json envelope = json::parse(data);
        msg.type = stringToMessageType(envelope.value("type", "Error"));
        msg.payload = envelope.value("payload", json::object());
    }
    catch (const json::exception&)
    {
        msg.type = MessageType::Error;
        msg.payload = {{"ok", false}, {"error", {{"code", "PARSE_ERROR"}, {"message", "Failed to parse JSON message"}}}};
    }
    return msg;
}

// --------------------- 统一命令协议层 ---------------------

// 统一的「命令」抽象：
// - name      : 命令名（例如 PING / MKDIR / LIST_PAPERS 等）
// - rawArgs   : 原始参数字符串（用于 WRITE 等需要保留空格的场景）
// - args      : 参数数组，适合大多数简单命令使用
// - sessionId : 可选，会话 ID
struct Command
{
    std::string              name;
    std::string              rawArgs;
    std::vector<std::string> args;
    std::string              sessionId; // 为空表示未携带 Session
};

// 从 JSON payload 解析 Command（用于服务端）
// JSON 格式: { "sessionId": "...", "cmd": "...", "args": [...], "rawArgs": "..." }
inline Command parseCommandFromJson(const json& payload)
{
    Command cmd;
    
    // sessionId 可能是 null 或字符串，需要特殊处理
    if (payload.contains("sessionId") && payload["sessionId"].is_string())
    {
        cmd.sessionId = payload["sessionId"].get<std::string>();
    }
    // 如果是 null 或不存在，sessionId 保持为空字符串
    
    cmd.name = payload.value("cmd", "");
    cmd.rawArgs = payload.value("rawArgs", "");

    if (payload.contains("args") && payload["args"].is_array())
    {
        for (const auto& arg : payload["args"])
        {
            if (arg.is_string())
            {
                cmd.args.push_back(arg.get<std::string>());
            }
        }
    }

    return cmd;
}

// 将 Command 转换为 JSON（用于客户端构建请求）
inline json commandToJson(const Command& cmd)
{
    json j;
    if (!cmd.sessionId.empty())
    {
        j["sessionId"] = cmd.sessionId;
    }
    else
    {
        j["sessionId"] = nullptr;
    }
    j["cmd"] = cmd.name;
    j["args"] = cmd.args;
    if (!cmd.rawArgs.empty())
    {
        j["rawArgs"] = cmd.rawArgs;
    }
    return j;
}

// --------------------- 响应构建辅助函数 ---------------------

// 构建成功响应
inline Message makeSuccessResponse(const json& data = json::object())
{
    Message msg;
    msg.type = MessageType::CommandResponse;
    msg.payload = {{"ok", true}, {"data", data}};
    return msg;
}

// 构建错误响应
inline Message makeErrorResponse(const std::string& code, const std::string& message, const json& details = json::object())
{
    Message msg;
    msg.type = MessageType::Error;
    json error = {{"code", code}, {"message", message}};
    if (!details.empty())
    {
        error["details"] = details;
    }
    msg.payload = {{"ok", false}, {"error", error}};
    return msg;
}

// --------------------- 兼容旧协议的辅助函数（可选，渐进迁移用）---------------------

// 解析单纯的命令行（不含 SESSION/CMD 前缀）：如 "MKDIR /demo"。
// 保留用于兼容旧文本格式（可在后续版本移除）
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

    // 按空格切分 rawArgs，得到 args
    std::istringstream iss(cmd.rawArgs);
    std::string        token;
    while (iss >> token)
    {
        cmd.args.push_back(token);
    }

    return cmd;
}

// 便于客户端构造命令：根据 Command 生成旧格式 payload 字符串
// 保留用于兼容旧文本格式（可在后续版本移除）
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

