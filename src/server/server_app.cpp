#include "server_app.hpp"

#include <sstream>

namespace osp::server
{

ServerApp::ServerApp(std::uint16_t port)
    : port_(port)
    , vfs_(64) // 简单设置缓存容量
    , auth_()
{
    // 初始化一些内置账号，便于本地测试与演示。
    // 注意：密码为明文，仅用于教学示例。
    auth_.addUser("admin", "admin", osp::Role::Admin);
    auth_.addUser("author", "author", osp::Role::Author);
    auth_.addUser("reviewer", "reviewer", osp::Role::Reviewer);
    auth_.addUser("editor", "editor", osp::Role::Editor);
}

void ServerApp::run()
{
    running_.store(true);
    osp::log(osp::LogLevel::Info, "Server starting on port " + std::to_string(port_));

    // 挂载简化 VFS
    vfs_.mount("data.fs");

    // 使用阻塞式 TCP 服务器，循环处理来自多个客户端的请求
    osp::net::TcpServer tcpServer(port_);
    while (running_.load())
    {
        const bool ok = tcpServer.serveOnce([this](const osp::protocol::Message& req) {
            return handleRequest(req);
        });

        if (!ok)
        {
            osp::log(osp::LogLevel::Warn, "TcpServer::serveOnce failed, stopping server loop");
            break;
        }
    }

    osp::log(osp::LogLevel::Info, "Server shutting down");
}

void ServerApp::stop()
{
    running_.store(false);
}

osp::protocol::Message ServerApp::handleRequest(const osp::protocol::Message& req)
{
    using osp::protocol::Message;
    using osp::protocol::MessageType;
    using osp::protocol::Command;

    if (req.type != MessageType::CommandRequest)
    {
        return {MessageType::Error, "Unsupported message type"};
    }

    osp::log(osp::LogLevel::Info, "Received request payload: " + req.payload);

    Command cmd = osp::protocol::parseCommandPayload(req.payload);
    if (cmd.name.empty())
    {
        return {MessageType::Error, "Empty command"};
    }

    // 如果携带了 Session 前缀，则在此统一校验会话是否有效
    std::optional<osp::domain::Session> maybeSession;
    if (!cmd.sessionId.empty())
    {
        auto s = auth_.validateSession(cmd.sessionId);
        if (!s)
        {
            return {MessageType::Error, "Invalid or expired session"};
        }
        maybeSession = *s;
    }

    return handleCommand(cmd, maybeSession);
}

osp::protocol::Message ServerApp::handleCommand(const osp::protocol::Command&                        cmd,
                                                const std::optional<osp::domain::Session>& maybeSession)
{
    using osp::protocol::Message;
    using osp::protocol::MessageType;

    // 预留：未来可以在这里增加鉴权 / 会话管理 / 日志审计等通用逻辑

    if (cmd.name == "PING")
    {
        return {MessageType::CommandResponse, "PONG"};
    }
    if (cmd.name == "LOGIN")
    {
        // LOGIN <username> <password>
        if (cmd.args.size() < 2)
        {
            return {MessageType::Error, "LOGIN: missing username or password"};
        }

        osp::Credentials cred;
        cred.username = cmd.args[0];
        cred.password = cmd.args[1];

        auto session = auth_.login(cred);
        if (!session)
        {
            return {MessageType::Error, "LOGIN failed: invalid credentials"};
        }

        // 简单文本响应：SESSION <id> USER <username> ROLE <role>
        std::string roleStr;
        switch (session->role)
        {
        case osp::Role::Author: roleStr = "Author"; break;
        case osp::Role::Reviewer: roleStr = "Reviewer"; break;
        case osp::Role::Editor: roleStr = "Editor"; break;
        case osp::Role::Admin: roleStr = "Admin"; break;
        }

        std::string payload =
            "SESSION " + session->id + " USER " + session->username + " ROLE " + roleStr;

        return {MessageType::CommandResponse, payload};
    }
    if (cmd.name == "LIST_PAPERS")
    {
        return {MessageType::CommandResponse, "No papers yet."};
    }

    // 文件系统相关命令，统一通过 handleFsCommand 处理
    if (cmd.name == "MKDIR" || cmd.name == "WRITE" || cmd.name == "READ" || cmd.name == "RM"
        || cmd.name == "RMDIR" || cmd.name == "LIST")
    {
        return handleFsCommand(cmd, maybeSession);
    }

    return {MessageType::Error, "Unknown command: " + cmd.name};
}

osp::protocol::Message
ServerApp::handleFsCommand(const osp::protocol::Command&                        cmd,
                           const std::optional<osp::domain::Session>& /*maybeSession*/)
{
    using osp::protocol::Message;
    using osp::protocol::MessageType;

    if (cmd.name == "MKDIR")
    {
        if (cmd.args.empty())
        {
            return {MessageType::Error, "MKDIR: missing path"};
        }
        const std::string& path = cmd.args[0];

        bool ok = vfs_.createDirectory(path);
        return ok ? Message{MessageType::CommandResponse, "MKDIR ok: " + path}
                  : Message{MessageType::Error, "MKDIR failed: " + path};
    }

    if (cmd.name == "WRITE")
    {
        // WRITE 命令需要保留路径之后的整行内容，因此使用 rawArgs 再次拆分
        if (cmd.rawArgs.empty())
        {
            return {MessageType::Error, "WRITE: missing path"};
        }

        std::istringstream iss(cmd.rawArgs);
        std::string         path;
        iss >> path;
        if (path.empty())
        {
            return {MessageType::Error, "WRITE: missing path"};
        }

        std::string content;
        std::getline(iss, content);
        if (!content.empty() && content.front() == ' ')
        {
            content.erase(content.begin());
        }

        bool ok = vfs_.writeFile(path, content);
        return ok ? Message{MessageType::CommandResponse, "WRITE ok: " + path}
                  : Message{MessageType::Error, "WRITE failed: " + path};
    }

    if (cmd.name == "READ")
    {
        if (cmd.args.empty())
        {
            return {MessageType::Error, "READ: missing path"};
        }
        const std::string& path = cmd.args[0];

        auto data = vfs_.readFile(path);
        if (!data)
        {
            return {MessageType::Error, "READ failed: " + path};
        }
        return {MessageType::CommandResponse, *data};
    }

    if (cmd.name == "RM")
    {
        if (cmd.args.empty())
        {
            return {MessageType::Error, "RM: missing path"};
        }
        const std::string& path = cmd.args[0];

        bool ok = vfs_.removeFile(path);
        return ok ? Message{MessageType::CommandResponse, "RM ok: " + path}
                  : Message{MessageType::Error, "RM failed: " + path};
    }

    if (cmd.name == "RMDIR")
    {
        if (cmd.args.empty())
        {
            return {MessageType::Error, "RMDIR: missing path"};
        }
        const std::string& path = cmd.args[0];

        bool ok = vfs_.removeDirectory(path);
        return ok ? Message{MessageType::CommandResponse, "RMDIR ok: " + path}
                  : Message{MessageType::Error, "RMDIR failed (maybe not empty?): " + path};
    }

    if (cmd.name == "LIST")
    {
        std::string path = "/";
        if (!cmd.args.empty())
        {
            path = cmd.args[0];
        }

        auto listing = vfs_.listDirectory(path);
        if (!listing)
        {
            return {MessageType::Error, "LIST failed: " + path};
        }
        return {MessageType::CommandResponse, *listing};
    }

    return {MessageType::Error, "Unknown FS command: " + cmd.name};
}

} // namespace osp::server


