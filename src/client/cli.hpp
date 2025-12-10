#pragma once

#include "common/protocol.hpp"

#include <string>

namespace osp::client
{

// 命令行界面（CLI）入口的简单封装
class Cli
{
public:
    Cli(std::string serverHost, unsigned short serverPort)
        : host_(std::move(serverHost))
        , port_(serverPort)
    {
    }

    // 启动交互式命令行循环。
    // - 支持 LOGIN 命令获取会话 ID（session）；
    // - 对后续业务命令自动在发送前附加 "SESSION <id> CMD ..." 前缀，用户无需手动输入 session。
    void run();

private:
    // 根据当前 sessionId 状态，将用户输入的一行命令封装为实际发送的 payload。
    // - 未登录或无需携带会话时：直接返回原始行（例如 PING / LOGIN）。
    // - 已登录且为普通业务命令时：自动封装为 "SESSION <sessionId> CMD <原始命令行>"。
    [[nodiscard]] std::string buildPayload(const std::string& line) const;

    // 当客户端发送 LOGIN 命令并收到成功响应时，从响应 payload 中解析并保存新的 sessionId。
    void handleLoginResponse(const std::string& requestLine,
                             const osp::protocol::Message& resp);

    // 根据角色打印对应的操作指引
    void printHelpForRole(const std::string& role) const;

private:
    std::string    host_;
    unsigned short port_{};
    std::string    sessionId_;   // 当前会话 ID，空字符串表示未登录
    std::string    currentPath_ = "/"; // 客户端维护的“当前目录”（仅影响默认 LIST 等命令）
};

} // namespace osp::client


