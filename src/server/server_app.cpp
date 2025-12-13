#include "server_app.hpp"

#include <sstream>

namespace osp::server
{

ServerApp::ServerApp(std::uint16_t port)
    : port_(port)
    , vfs_(64) // 简单设置缓存容量
    , auth_()
{
    // 添加默认用户
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

    // 编辑相关命令
    if (cmd.name == "ASSIGN_REVIEWER")
    {
        // ASSIGN_REVIEWER <paper_id> <reviewer_username>
        if (cmd.args.size() < 2)
        {
            return {MessageType::Error, "ASSIGN_REVIEWER: missing paper_id or reviewer_username"};
        }
        if (!maybeSession)
        {
            return {MessageType::Error, "ASSIGN_REVIEWER: need to login first"};
        }
        if (maybeSession->role != osp::Role::Editor)
        {
            return {MessageType::Error, "ASSIGN_REVIEWER: permission denied"};
        }
        return {MessageType::CommandResponse, "ASSIGN_REVIEWER ok: paper " + cmd.args[0] + " to reviewer " + cmd.args[1]};
    }
    if (cmd.name == "VIEW_REVIEW_STATUS")
    {
        // VIEW_REVIEW_STATUS <paper_id>
        if (cmd.args.size() < 1)
        {
            return {MessageType::Error, "VIEW_REVIEW_STATUS: missing paper_id"};
        }
        if (!maybeSession)
        {
            return {MessageType::Error, "VIEW_REVIEW_STATUS: need to login first"};
        }
        if (maybeSession->role != osp::Role::Editor)
        {
            return {MessageType::Error, "VIEW_REVIEW_STATUS: permission denied"};
        }
        return {MessageType::CommandResponse, "VIEW_REVIEW_STATUS ok: paper " + cmd.args[0] + " status: pending"};
    }
    if (cmd.name == "MAKE_FINAL_DECISION")
    {
        // MAKE_FINAL_DECISION <paper_id> <decision>
        if (cmd.args.size() < 2)
        {
            return {MessageType::Error, "MAKE_FINAL_DECISION: missing paper_id or decision"};
        }
        if (!maybeSession)
        {
            return {MessageType::Error, "MAKE_FINAL_DECISION: need to login first"};
        }
        if (maybeSession->role != osp::Role::Editor)
        {
            return {MessageType::Error, "MAKE_FINAL_DECISION: permission denied"};
        }
        return {MessageType::CommandResponse, "MAKE_FINAL_DECISION ok: paper " + cmd.args[0] + " decision: " + cmd.args[1]};
    }

    // 管理员相关命令
    if (cmd.name == "MANAGE_USERS")
    {
        // MANAGE_USERS <subcommand> [args]
        if (cmd.args.size() < 1)
        {
            return {MessageType::Error, "MANAGE_USERS: missing subcommand"};
        }
        if (!maybeSession)
        {
            return {MessageType::Error, "MANAGE_USERS: need to login first"};
        }
        if (maybeSession->role != osp::Role::Admin)
        {
            return {MessageType::Error, "MANAGE_USERS: permission denied"};
        }
        
        const std::string& subcmd = cmd.args[0];
        if (subcmd == "LIST")
        {
            auto users = auth_.getAllUsers();
            std::string response = "Users list:\n";
            for (const auto& user : users)
            {
                std::string roleStr;
                switch (user.role())
                {
                case osp::Role::Author: roleStr = "Author";
                    break;
                case osp::Role::Reviewer: roleStr = "Reviewer";
                    break;
                case osp::Role::Editor: roleStr = "Editor";
                    break;
                case osp::Role::Admin: roleStr = "Admin";
                    break;
                }
                response += "- " + user.username() + " (ID: " + std::to_string(user.id()) + ", Role: " + roleStr + ")\n";
            }
            return {MessageType::CommandResponse, response};
        }
        else if (subcmd == "ADD")
        {
            // MANAGE_USERS ADD <username> <password> <role>
            if (cmd.args.size() < 4)
            {
                return {MessageType::Error, "MANAGE_USERS ADD: missing username, password, or role"};
            }
            const std::string& username = cmd.args[1];
            const std::string& password = cmd.args[2];
            const std::string& roleStr = cmd.args[3];
            
            osp::Role role = osp::Role::Author;
            if (roleStr == "Reviewer") role = osp::Role::Reviewer;
            else if (roleStr == "Editor") role = osp::Role::Editor;
            else if (roleStr == "Admin") role = osp::Role::Admin;
            
            auth_.addUser(username, password, role);
            return {MessageType::CommandResponse, "MANAGE_USERS ADD ok: " + username};
        }
        else if (subcmd == "REMOVE")
        {
            // MANAGE_USERS REMOVE <username>
            if (cmd.args.size() < 2)
            {
                return {MessageType::Error, "MANAGE_USERS REMOVE: missing username"};
            }
            const std::string& username = cmd.args[1];
            if (auth_.removeUser(username))
            {
                return {MessageType::CommandResponse, "MANAGE_USERS REMOVE ok: " + username};
            }
            else
            {
                return {MessageType::Error, "MANAGE_USERS REMOVE failed: user not found"};
            }
        }
        else if (subcmd == "UPDATE_ROLE")
        {
            // MANAGE_USERS UPDATE_ROLE <username> <role>
            if (cmd.args.size() < 3)
            {
                return {MessageType::Error, "MANAGE_USERS UPDATE_ROLE: missing username or role"};
            }
            const std::string& username = cmd.args[1];
            const std::string& roleStr = cmd.args[2];
            
            osp::Role role = osp::Role::Author;
            if (roleStr == "Reviewer") role = osp::Role::Reviewer;
            else if (roleStr == "Editor") role = osp::Role::Editor;
            else if (roleStr == "Admin") role = osp::Role::Admin;
            
            if (auth_.updateUserRole(username, role))
            {
                return {MessageType::CommandResponse, "MANAGE_USERS UPDATE_ROLE ok: " + username + " to " + roleStr};
            }
            else
            {
                return {MessageType::Error, "MANAGE_USERS UPDATE_ROLE failed: user not found"};
            }
        }
        else if (subcmd == "RESET_PASSWORD")
        {
            // MANAGE_USERS RESET_PASSWORD <username> <new_password>
            if (cmd.args.size() < 3)
            {
                return {MessageType::Error, "MANAGE_USERS RESET_PASSWORD: missing username or new_password"};
            }
            const std::string& username = cmd.args[1];
            const std::string& newPassword = cmd.args[2];
            
            if (auth_.resetUserPassword(username, newPassword))
            {
                return {MessageType::CommandResponse, "MANAGE_USERS RESET_PASSWORD ok: " + username};
            }
            else
            {
                return {MessageType::Error, "MANAGE_USERS RESET_PASSWORD failed: user not found"};
            }
        }
        else
        {
            return {MessageType::Error, "MANAGE_USERS: unknown subcommand " + subcmd};
        }
    }
    if (cmd.name == "BACKUP")
    {
        // BACKUP <path>
        if (cmd.args.size() < 1)
        {
            return {MessageType::Error, "BACKUP: missing path"};
        }
        if (!maybeSession)
        {
            return {MessageType::Error, "BACKUP: need to login first"};
        }
        if (maybeSession->role != osp::Role::Admin)
        {
            return {MessageType::Error, "BACKUP: permission denied"};
        }
        return {MessageType::CommandResponse, "BACKUP ok: " + cmd.args[0]};
    }
    if (cmd.name == "RESTORE")
    {
        // RESTORE <path>
        if (cmd.args.size() < 1)
        {
            return {MessageType::Error, "RESTORE: missing path"};
        }
        if (!maybeSession)
        {
            return {MessageType::Error, "RESTORE: need to login first"};
        }
        if (maybeSession->role != osp::Role::Admin)
        {
            return {MessageType::Error, "RESTORE: permission denied"};
        }
        return {MessageType::CommandResponse, "RESTORE ok: " + cmd.args[0]};
    }
    if (cmd.name == "VIEW_SYSTEM_STATUS")
    {
        // VIEW_SYSTEM_STATUS
        if (!maybeSession)
        {
            return {MessageType::Error, "VIEW_SYSTEM_STATUS: need to login first"};
        }
        if (maybeSession->role != osp::Role::Admin && maybeSession->role != osp::Role::Editor)
        {
            return {MessageType::Error, "VIEW_SYSTEM_STATUS: permission denied"};
        }
        return {MessageType::CommandResponse, "System status:\n- Users: 4\n- Papers: 0\n- Reviews: 0\n- Sessions: " + std::to_string(1)};
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




