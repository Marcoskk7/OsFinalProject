#include "server_app.hpp"

#include "domain/paper.hpp"
#include "domain/permissions.hpp"
#include "domain/review.hpp"

#include <sstream>
#include <cstdlib>
#include <fstream>
#include <filesystem>

namespace osp::server
{

namespace
{
// Role 转字符串
std::string roleToString(osp::Role role)
{
    switch (role)
    {
    case osp::Role::Author: return "Author";
    case osp::Role::Reviewer: return "Reviewer";
    case osp::Role::Editor: return "Editor";
    case osp::Role::Admin: return "Admin";
    }
    return "Unknown";
}

// 字符串转 Role
osp::Role stringToRole(const std::string& s)
{
    if (s == "Reviewer") return osp::Role::Reviewer;
    if (s == "Editor") return osp::Role::Editor;
    if (s == "Admin") return osp::Role::Admin;
    return osp::Role::Author;
}

std::size_t clampCacheCapacity(std::size_t v)
{
    // 防止极端值导致内存占用过大；课程项目里给一个温和上限即可
    constexpr std::size_t kMax = 4096;
    if (v > kMax) return kMax;
    return v;
}
} // namespace

ServerApp::ServerApp(std::uint16_t port, std::size_t cacheCapacity)
    : port_(port)
    , vfs_(clampCacheCapacity(cacheCapacity))
    , auth_()
{
    // 初始化一些内置账号，便于本地测试与演示。
    // 注意：密码为明文，仅用于教学示例。
    auth_.addUser("admin", "admin", osp::Role::Admin);
    auth_.addUser("author", "author", osp::Role::Author);
    auth_.addUser("author2", "author2", osp::Role::Author); // 添加第二个作者用于测试权限隔离
    auth_.addUser("reviewer", "reviewer", osp::Role::Reviewer);
    auth_.addUser("editor", "editor", osp::Role::Editor);
}

void ServerApp::run()
{
    running_.store(true);
    osp::log(osp::LogLevel::Info,
             "Server starting on port " + std::to_string(port_) + " (cacheCapacity="
                 + std::to_string(vfs_.cacheCapacity()) + ")");

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
    using osp::protocol::json;

    if (req.type != MessageType::CommandRequest)
    {
        return osp::protocol::makeErrorResponse("INVALID_TYPE", "Unsupported message type");
    }

    osp::log(osp::LogLevel::Info, "Received request payload: " + req.payload.dump());

    // 从 JSON payload 解析 Command
    Command cmd = osp::protocol::parseCommandFromJson(req.payload);
    if (cmd.name.empty())
    {
        return osp::protocol::makeErrorResponse("EMPTY_COMMAND", "Empty command");
    }

    // 如果携带了 Session ID，则在此统一校验会话是否有效
    std::optional<osp::domain::Session> maybeSession;
    if (!cmd.sessionId.empty())
    {
        auto s = auth_.validateSession(cmd.sessionId);
        if (!s)
        {
            return osp::protocol::makeErrorResponse("INVALID_SESSION", "Invalid or expired session");
        }
        maybeSession = *s;
    }

    return handleCommand(cmd, maybeSession);
}

osp::protocol::Message ServerApp::handleCommand(const osp::protocol::Command&           cmd,
                                                const std::optional<osp::domain::Session>& maybeSession)
{
    using osp::protocol::Message;
    using osp::protocol::MessageType;
    using osp::protocol::json;

    // PING
    if (cmd.name == "PING")
    {
        return osp::protocol::makeSuccessResponse({{"message", "PONG"}});
    }

    // LOGIN
    if (cmd.name == "LOGIN")
    {
        if (cmd.args.size() < 2)
        {
            return osp::protocol::makeErrorResponse("MISSING_ARGS", "LOGIN: missing username or password");
        }

        osp::Credentials cred;
        cred.username = cmd.args[0];
        cred.password = cmd.args[1];

        auto session = auth_.login(cred);
        if (!session)
        {
            return osp::protocol::makeErrorResponse("LOGIN_FAILED", "LOGIN failed: invalid credentials");
        }

        json data;
        data["sessionId"] = session->id;
        data["userId"] = session->userId;
        data["username"] = session->username;
        data["role"] = roleToString(session->role);

        return osp::protocol::makeSuccessResponse(data);
    }

    // 论文相关命令
    if (cmd.name == "LIST_PAPERS" || cmd.name == "SUBMIT" || cmd.name == "SUBMIT_REVISION" || cmd.name == "GET_PAPER"
        || cmd.name == "ASSIGN" || cmd.name == "REVIEW" || cmd.name == "LIST_REVIEWS"
        || cmd.name == "DECISION")
    {
        return handlePaperCommand(cmd, maybeSession);
    }

    // Editor 便捷封装命令
    if (cmd.name == "ASSIGN_REVIEWER")
    {
        if (cmd.args.size() < 2)
        {
            return osp::protocol::makeErrorResponse("MISSING_ARGS", "ASSIGN_REVIEWER: missing paper_id or reviewer_username");
        }
        osp::protocol::Command assignCmd = cmd;
        assignCmd.name = "ASSIGN";
        return handlePaperCommand(assignCmd, maybeSession);
    }
    if (cmd.name == "VIEW_REVIEW_STATUS")
    {
        if (cmd.args.empty())
        {
            return osp::protocol::makeErrorResponse("MISSING_ARGS", "VIEW_REVIEW_STATUS: missing paper_id");
        }
        osp::protocol::Command listCmd = cmd;
        listCmd.name = "LIST_REVIEWS";
        return handlePaperCommand(listCmd, maybeSession);
    }
    if (cmd.name == "MAKE_FINAL_DECISION")
    {
        if (cmd.args.size() < 2)
        {
            return osp::protocol::makeErrorResponse("MISSING_ARGS", "MAKE_FINAL_DECISION: missing paper_id or decision");
        }
        osp::protocol::Command decisionCmd = cmd;
        decisionCmd.name = "DECISION";
        return handlePaperCommand(decisionCmd, maybeSession);
    }

    // 管理员相关命令
    if (cmd.name == "MANAGE_USERS")
    {
        if (cmd.args.empty())
        {
            return osp::protocol::makeErrorResponse("MISSING_ARGS", "MANAGE_USERS: missing subcommand");
        }
        if (!maybeSession)
        {
            return osp::protocol::makeErrorResponse("AUTH_REQUIRED", "MANAGE_USERS: need to login first");
        }
        if (maybeSession->role != osp::Role::Admin)
        {
            return osp::protocol::makeErrorResponse("PERMISSION_DENIED", "MANAGE_USERS: permission denied");
        }
        
        const std::string& subcmd = cmd.args[0];
        if (subcmd == "LIST")
        {
            auto users = auth_.getAllUsers();
            json userList = json::array();
            for (const auto& user : users)
            {
                userList.push_back({
                    {"id", user.id()},
                    {"username", user.username()},
                    {"role", roleToString(user.role())}
                });
            }
            return osp::protocol::makeSuccessResponse({{"users", userList}});
        }
        else if (subcmd == "ADD")
        {
            if (cmd.args.size() < 4)
            {
                return osp::protocol::makeErrorResponse("MISSING_ARGS", "MANAGE_USERS ADD: missing username, password, or role");
            }
            const std::string& username = cmd.args[1];
            const std::string& password = cmd.args[2];
            const std::string& roleStr = cmd.args[3];
            
            osp::Role role = stringToRole(roleStr);
            auth_.addUser(username, password, role);
            return osp::protocol::makeSuccessResponse({{"message", "User added"}, {"username", username}});
        }
        else if (subcmd == "REMOVE")
        {
            if (cmd.args.size() < 2)
            {
                return osp::protocol::makeErrorResponse("MISSING_ARGS", "MANAGE_USERS REMOVE: missing username");
            }
            const std::string& username = cmd.args[1];
            if (auth_.removeUser(username))
            {
                return osp::protocol::makeSuccessResponse({{"message", "User removed"}, {"username", username}});
            }
            else
            {
                return osp::protocol::makeErrorResponse("NOT_FOUND", "MANAGE_USERS REMOVE failed: user not found");
            }
        }
        else if (subcmd == "UPDATE_ROLE")
        {
            if (cmd.args.size() < 3)
            {
                return osp::protocol::makeErrorResponse("MISSING_ARGS", "MANAGE_USERS UPDATE_ROLE: missing username or role");
            }
            const std::string& username = cmd.args[1];
            const std::string& roleStr = cmd.args[2];
            
            osp::Role role = stringToRole(roleStr);
            if (auth_.updateUserRole(username, role))
            {
                return osp::protocol::makeSuccessResponse({{"message", "Role updated"}, {"username", username}, {"role", roleStr}});
            }
            else
            {
                return osp::protocol::makeErrorResponse("NOT_FOUND", "MANAGE_USERS UPDATE_ROLE failed: user not found");
            }
        }
        else if (subcmd == "RESET_PASSWORD")
        {
            if (cmd.args.size() < 3)
            {
                return osp::protocol::makeErrorResponse("MISSING_ARGS", "MANAGE_USERS RESET_PASSWORD: missing username or new_password");
            }
            const std::string& username = cmd.args[1];
            const std::string& newPassword = cmd.args[2];
            
            if (auth_.resetUserPassword(username, newPassword))
            {
                return osp::protocol::makeSuccessResponse({{"message", "Password reset"}, {"username", username}});
            }
            else
            {
                return osp::protocol::makeErrorResponse("NOT_FOUND", "MANAGE_USERS RESET_PASSWORD failed: user not found");
            }
        }
        else
        {
            return osp::protocol::makeErrorResponse("UNKNOWN_SUBCMD", "MANAGE_USERS: unknown subcommand " + subcmd);
        }
    }

    if (cmd.name == "BACKUP")
    {
        if (cmd.args.empty())
        {
            return osp::protocol::makeErrorResponse("MISSING_ARGS", "BACKUP: missing path");
        }
        if (!maybeSession)
        {
            return osp::protocol::makeErrorResponse("AUTH_REQUIRED", "BACKUP: need to login first");
        }
        if (maybeSession->role != osp::Role::Admin)
        {
            return osp::protocol::makeErrorResponse("PERMISSION_DENIED", "BACKUP: permission denied");
        }

        const std::string& backupPath = cmd.args[0];
        const std::string sourceFile = "data.fs";

        namespace fs = std::filesystem;

        try
        {
            // 检查源文件是否存在
            if (!fs::exists(sourceFile))
            {
                return osp::protocol::makeErrorResponse("NOT_FOUND", "Source file not found: " + sourceFile);
            }

            // 确保备份目录存在
            fs::path backupFilePath(backupPath);
            fs::path backupDir = backupFilePath.parent_path();
            if (!backupDir.empty() && !fs::exists(backupDir))
            {
                fs::create_directories(backupDir);
            }

            // 复制文件
            fs::copy_file(sourceFile, backupPath, fs::copy_options::overwrite_existing);

            return osp::protocol::makeSuccessResponse({
                {"message", "Backup completed successfully"},
                {"source", sourceFile},
                {"backup", backupPath},
                {"size", static_cast<std::uint64_t>(fs::file_size(backupPath))}
            });
        }
        catch (const fs::filesystem_error& e)
        {
            return osp::protocol::makeErrorResponse("FS_ERROR", "Backup failed: " + std::string(e.what()));
        }
        catch (const std::exception& e)
        {
            return osp::protocol::makeErrorResponse("ERROR", "Backup failed: " + std::string(e.what()));
        }
    }

    if (cmd.name == "RESTORE")
    {
        if (cmd.args.empty())
        {
            return osp::protocol::makeErrorResponse("MISSING_ARGS", "RESTORE: missing path");
        }
        if (!maybeSession)
        {
            return osp::protocol::makeErrorResponse("AUTH_REQUIRED", "RESTORE: need to login first");
        }
        if (maybeSession->role != osp::Role::Admin)
        {
            return osp::protocol::makeErrorResponse("PERMISSION_DENIED", "RESTORE: permission denied");
        }

        const std::string& backupPath = cmd.args[0];
        const std::string targetFile = "data.fs";

        namespace fs = std::filesystem;

        try
        {
            // 检查备份文件是否存在
            if (!fs::exists(backupPath))
            {
                return osp::protocol::makeErrorResponse("NOT_FOUND", "Backup file not found: " + backupPath);
            }

            // 检查备份文件大小（简单验证）
            if (fs::file_size(backupPath) == 0)
            {
                return osp::protocol::makeErrorResponse("INVALID_BACKUP", "Backup file is empty");
            }

            // 如果目标文件存在，先备份当前版本（安全措施）
            if (fs::exists(targetFile))
            {
                std::string safetyBackup = targetFile + ".pre_restore";
                fs::copy_file(targetFile, safetyBackup, fs::copy_options::overwrite_existing);
            }

            // 复制备份文件到目标位置
            fs::copy_file(backupPath, targetFile, fs::copy_options::overwrite_existing);

            return osp::protocol::makeSuccessResponse({
                {"message", "Restore completed successfully. Server restart required to load restored filesystem."},
                {"backup", backupPath},
                {"target", targetFile},
                {"size", static_cast<std::uint64_t>(fs::file_size(targetFile))},
                {"warning", "Please restart the server to use the restored filesystem"}
            });
        }
        catch (const fs::filesystem_error& e)
        {
            return osp::protocol::makeErrorResponse("FS_ERROR", "Restore failed: " + std::string(e.what()));
        }
        catch (const std::exception& e)
        {
            return osp::protocol::makeErrorResponse("ERROR", "Restore failed: " + std::string(e.what()));
        }
    }

    if (cmd.name == "VIEW_SYSTEM_STATUS")
    {
        if (!maybeSession)
        {
            return osp::protocol::makeErrorResponse("AUTH_REQUIRED", "VIEW_SYSTEM_STATUS: need to login first");
        }
        if (maybeSession->role != osp::Role::Admin && maybeSession->role != osp::Role::Editor)
        {
            return osp::protocol::makeErrorResponse("PERMISSION_DENIED", "VIEW_SYSTEM_STATUS: permission denied");
        }

        const std::size_t userCount = auth_.getAllUsers().size();
        const std::size_t sessionCount = auth_.sessionCount();

        // Papers: 通过遍历 /papers/<id>/ 目录计数（只统计一级目录项）
        std::size_t paperCount = 0;
        auto papersListing = vfs_.listDirectory("/papers");
        if (papersListing)
        {
            std::stringstream ss(*papersListing);
            std::string       entry;
            while (std::getline(ss, entry))
            {
                if (!entry.empty() && entry.back() == '/')
                {
                    ++paperCount;
                }
            }
        }

        // Reviews: 遍历每篇论文的 /reviews 目录内文件数量
        std::size_t reviewCount = 0;
        if (papersListing)
        {
            std::stringstream ss(*papersListing);
            std::string       entry;
            while (std::getline(ss, entry))
            {
                if (entry.empty() || entry.back() != '/')
                {
                    continue;
                }

                const std::string pidStr = entry.substr(0, entry.size() - 1);
                const std::string reviewsDir = "/papers/" + pidStr + "/reviews";
                auto reviewsListing = vfs_.listDirectory(reviewsDir);
                if (!reviewsListing)
                {
                    continue;
                }
                std::stringstream rss(*reviewsListing);
                std::string       f;
                while (std::getline(rss, f))
                {
                    if (!f.empty() && f.back() != '/')
                    {
                        ++reviewCount;
                    }
                }
            }
        }

        const auto cs = vfs_.cacheStats();
        json       data;
        data["users"] = userCount;
        data["sessions"] = sessionCount;
        data["papers"] = paperCount;
        data["reviews"] = reviewCount;
        data["blockCache"] = {
            {"capacity", cs.capacity},
            {"entries", cs.entries},
            {"hits", cs.hits},
            {"misses", cs.misses},
            {"replacements", cs.replacements}
        };

        return osp::protocol::makeSuccessResponse(data);
    }

    // 文件系统相关命令
    if (cmd.name == "MKDIR" || cmd.name == "WRITE" || cmd.name == "READ" || cmd.name == "RM"
        || cmd.name == "RMDIR" || cmd.name == "LIST")
    {
        return handleFsCommand(cmd, maybeSession);
    }

    return osp::protocol::makeErrorResponse("UNKNOWN_COMMAND", "Unknown command: " + cmd.name);
}

osp::protocol::Message
ServerApp::handlePaperCommand(const osp::protocol::Command&                        cmd,
                              const std::optional<osp::domain::Session>& maybeSession)
{
    using osp::protocol::Message;
    using osp::protocol::MessageType;
    using osp::protocol::json;
    using osp::domain::Permission;
    using osp::domain::hasPermission;

    if (!maybeSession)
    {
        return osp::protocol::makeErrorResponse("AUTH_REQUIRED", "Authentication required");
    }

    if (cmd.name == "LIST_PAPERS")
    {
        bool isAuthor   = (maybeSession->role == osp::Role::Author);
        bool isReviewer = (maybeSession->role == osp::Role::Reviewer);

        if (isAuthor && !hasPermission(maybeSession->role, Permission::ViewOwnPaperStatus))
        {
            return osp::protocol::makeErrorResponse("PERMISSION_DENIED", "Permission denied");
        }
        if (isReviewer && !hasPermission(maybeSession->role, Permission::DownloadAssignedPapers))
        {
            return osp::protocol::makeErrorResponse("PERMISSION_DENIED", "Permission denied");
        }

        auto listing = vfs_.listDirectory("/papers");
        if (!listing)
        {
            return osp::protocol::makeSuccessResponse({{"papers", json::array()}});
        }

        std::stringstream ss(*listing);
        std::string       entry;
        json              papers = json::array();

        while (std::getline(ss, entry))
        {
            if (entry.empty() || entry.back() != '/')
            {
                continue;
            }

            std::string pidStr   = entry.substr(0, entry.size() - 1);
            std::string metaPath = "/papers/" + pidStr + "/meta.txt";

            auto metaData = vfs_.readFile(metaPath);
            if (!metaData)
            {
                continue;
            }

            std::stringstream metaSS(*metaData);
            std::uint32_t     p_id;
            std::uint32_t     p_authorId;
            std::string       p_status;
            std::string       p_title;

            if (!(metaSS >> p_id >> p_authorId >> p_status))
            {
                continue;
            }
            char dummy;
            metaSS.get(dummy);
            std::getline(metaSS, p_title);

            // Author 只能看自己的
            if (isAuthor && p_authorId != maybeSession->userId)
            {
                continue;
            }

            // Reviewer 只能看分配给自己的
            if (isReviewer)
            {
                std::string reviewersPath = "/papers/" + pidStr + "/reviewers.txt";
                auto        reviewersData = vfs_.readFile(reviewersPath);
                bool        assigned      = false;
                if (reviewersData)
                {
                    std::stringstream rss(*reviewersData);
                    std::string       rid;
                    std::string       myIdStr = std::to_string(maybeSession->userId);
                    while (rss >> rid)
                    {
                        if (rid == myIdStr)
                        {
                            assigned = true;
                            break;
                        }
                    }
                }
                if (!assigned)
                {
                    continue;
                }
            }

            papers.push_back({
                {"id", p_id},
                {"title", p_title},
                {"status", p_status},
                {"authorId", p_authorId}
            });
        }

        return osp::protocol::makeSuccessResponse({{"papers", papers}});
    }

    if (cmd.name == "GET_PAPER")
    {
        if (cmd.args.empty())
        {
            return osp::protocol::makeErrorResponse("MISSING_ARGS", "Usage: GET_PAPER <PaperID>");
        }

        std::string pidStr   = cmd.args[0];
        std::string metaPath = "/papers/" + pidStr + "/meta.txt";

        auto metaData = vfs_.readFile(metaPath);
        if (!metaData)
        {
            return osp::protocol::makeErrorResponse("NOT_FOUND", "Paper not found");
        }

        std::stringstream metaSS(*metaData);
        std::uint32_t     p_id;
        std::uint32_t     p_authorId;
        std::string       p_status;
        std::string       p_title;

        metaSS >> p_id >> p_authorId >> p_status;
        char dummy;
        metaSS.get(dummy);
        std::getline(metaSS, p_title);

        if (maybeSession->role == osp::Role::Author && p_authorId != maybeSession->userId)
        {
            return osp::protocol::makeErrorResponse("PERMISSION_DENIED", "Permission denied: You can only view your own papers");
        }

        if (maybeSession->role == osp::Role::Reviewer)
        {
            std::string reviewersPath = "/papers/" + pidStr + "/reviewers.txt";
            auto        reviewersData = vfs_.readFile(reviewersPath);
            bool        assigned      = false;
            if (reviewersData)
            {
                std::stringstream rss(*reviewersData);
                std::string       rid;
                std::string       myIdStr = std::to_string(maybeSession->userId);
                while (rss >> rid)
                {
                    if (rid == myIdStr)
                    {
                        assigned = true;
                        break;
                    }
                }
            }
            if (!assigned)
            {
                return osp::protocol::makeErrorResponse("PERMISSION_DENIED", "Permission denied: You are not assigned to this paper");
            }
        }

        std::string contentPath = "/papers/" + pidStr + "/content.txt";
        auto        contentData = vfs_.readFile(contentPath);

        json data;
        data["id"] = p_id;
        data["title"] = p_title;
        data["status"] = p_status;
        data["authorId"] = p_authorId;
        data["content"] = contentData ? *contentData : "";

        return osp::protocol::makeSuccessResponse(data);
    }

    if (cmd.name == "SUBMIT")
    {
        if (!hasPermission(maybeSession->role, Permission::UploadPaper))
        {
            return osp::protocol::makeErrorResponse("PERMISSION_DENIED", "Permission denied: Author role required");
        }

        if (cmd.args.size() < 2)
        {
            return osp::protocol::makeErrorResponse("MISSING_ARGS", "Usage: SUBMIT <Title> <Content>");
        }

        std::string title = cmd.args[0];

        // 从 rawArgs 中提取 content
        std::string content;
        size_t      titlePos = cmd.rawArgs.find(title);
        if (titlePos != std::string::npos)
        {
            size_t contentStart = titlePos + title.length();
            while (contentStart < cmd.rawArgs.length()
                   && std::isspace(static_cast<unsigned char>(cmd.rawArgs[contentStart])))
            {
                contentStart++;
            }
            content = cmd.rawArgs.substr(contentStart);
        }

        if (content.empty())
        {
            return osp::protocol::makeErrorResponse("INVALID_ARGS", "SUBMIT: Content is empty");
        }

        std::uint32_t pid = nextPaperId();
        std::string paperDir = "/papers/" + std::to_string(pid);

        vfs_.createDirectory("/papers");

        if (!vfs_.createDirectory(paperDir))
        {
            return osp::protocol::makeErrorResponse("FS_ERROR", "Failed to create paper directory");
        }

        if (!vfs_.writeFile(paperDir + "/content.txt", content))
        {
            return osp::protocol::makeErrorResponse("FS_ERROR", "Failed to save paper content");
        }

        std::ostringstream meta;
        meta << pid << "\n"
             << maybeSession->userId << "\n"
             << osp::domain::paperStatusToString(osp::domain::PaperStatus::Submitted) << "\n"
             << title;

        if (!vfs_.writeFile(paperDir + "/meta.txt", meta.str()))
        {
            return osp::protocol::makeErrorResponse("FS_ERROR", "Failed to save paper metadata");
        }

        return osp::protocol::makeSuccessResponse({{"message", "Paper submitted successfully"}, {"paperId", pid}});
    }

    if (cmd.name == "SUBMIT_REVISION")
    {
        if (!hasPermission(maybeSession->role, Permission::SubmitRevision))
        {
            return osp::protocol::makeErrorResponse("PERMISSION_DENIED", "Permission denied: Author role required to submit revision");
        }

        if (cmd.args.size() < 2)
        {
            return osp::protocol::makeErrorResponse("MISSING_ARGS", "Usage: SUBMIT_REVISION <PaperID> <Content>");
        }

        std::string pidStr = cmd.args[0];
        std::string paperDir = "/papers/" + pidStr;
        std::string metaPath = paperDir + "/meta.txt";

        // 检查论文是否存在
        auto metaData = vfs_.readFile(metaPath);
        if (!metaData)
        {
            return osp::protocol::makeErrorResponse("NOT_FOUND", "Paper not found: " + pidStr);
        }

        // 解析论文元数据，检查作者权限
        std::stringstream metaSS(*metaData);
        std::uint32_t p_id;
        std::uint32_t p_authorId;
        std::string p_status;
        std::string p_title;

        metaSS >> p_id >> p_authorId >> p_status;
        char dummy;
        metaSS.get(dummy);
        std::getline(metaSS, p_title);

        // 检查是否是论文作者
        if (p_authorId != maybeSession->userId)
        {
            return osp::protocol::makeErrorResponse("PERMISSION_DENIED", "Permission denied: You can only submit revisions for your own papers");
        }

        // 从 rawArgs 中提取修订内容
        std::string content;
        size_t pidPos = cmd.rawArgs.find(pidStr);
        if (pidPos != std::string::npos)
        {
            size_t contentStart = pidPos + pidStr.length();
            while (contentStart < cmd.rawArgs.length()
                   && std::isspace(static_cast<unsigned char>(cmd.rawArgs[contentStart])))
            {
                contentStart++;
            }
            content = cmd.rawArgs.substr(contentStart);
        }

        if (content.empty())
        {
            return osp::protocol::makeErrorResponse("INVALID_ARGS", "SUBMIT_REVISION: Content is empty");
        }

        // 创建 revisions 目录（如果不存在）
        std::string revisionsDir = paperDir + "/revisions";
        vfs_.createDirectory(revisionsDir);

        // 读取当前版本号
        std::string versionFile = revisionsDir + "/version.txt";
        std::uint32_t version = 1;
        auto versionData = vfs_.readFile(versionFile);
        if (versionData)
        {
            try
            {
                version = std::stoul(*versionData) + 1;
            }
            catch (...)
            {
                version = 1;
            }
        }

        // 保存新版本
        std::string revisionFile = revisionsDir + "/v" + std::to_string(version) + ".txt";
        if (!vfs_.writeFile(revisionFile, content))
        {
            return osp::protocol::makeErrorResponse("FS_ERROR", "Failed to save revision content");
        }

        // 更新版本号
        if (!vfs_.writeFile(versionFile, std::to_string(version)))
        {
            return osp::protocol::makeErrorResponse("FS_ERROR", "Failed to update version number");
        }

        // 更新主内容文件（将修订版本作为当前版本）
        if (!vfs_.writeFile(paperDir + "/content.txt", content))
        {
            return osp::protocol::makeErrorResponse("FS_ERROR", "Failed to update paper content");
        }

        // 更新论文状态为 Submitted（如果之前被拒绝或需要修订）
        if (p_status == "Rejected" || p_status == "UnderReview")
        {
            std::ostringstream newMeta;
            newMeta << p_id << "\n"
                    << p_authorId << "\n"
                    << osp::domain::paperStatusToString(osp::domain::PaperStatus::Submitted) << "\n"
                    << p_title;
            if (!vfs_.writeFile(metaPath, newMeta.str()))
            {
                return osp::protocol::makeErrorResponse("FS_ERROR", "Failed to update paper status");
            }
            p_status = "Submitted";
        }

        return osp::protocol::makeSuccessResponse({
            {"message", "Revision submitted successfully"},
            {"paperId", p_id},
            {"version", version},
            {"status", p_status}
        });
    }

    if (cmd.name == "ASSIGN")
    {
        if (!hasPermission(maybeSession->role, Permission::AssignReviewers))
        {
            return osp::protocol::makeErrorResponse("PERMISSION_DENIED", "Permission denied: Editor role required");
        }

        if (cmd.args.size() < 2)
        {
            return osp::protocol::makeErrorResponse("MISSING_ARGS", "Usage: ASSIGN <PaperID> <ReviewerUsername>");
        }

        std::string pidStr       = cmd.args[0];
        std::string reviewerName = cmd.args[1];

        std::string paperDir = "/papers/" + pidStr;
        std::string metaPath = paperDir + "/meta.txt";
        if (!vfs_.readFile(metaPath))
        {
            return osp::protocol::makeErrorResponse("NOT_FOUND", "Paper not found: " + pidStr);
        }

        auto reviewerIdOpt = auth_.getUserId(reviewerName);
        if (!reviewerIdOpt)
        {
            return osp::protocol::makeErrorResponse("NOT_FOUND", "User not found: " + reviewerName);
        }

        std::string reviewersPath   = paperDir + "/reviewers.txt";
        std::string currentReviewers;
        auto        existing = vfs_.readFile(reviewersPath);
        if (existing)
        {
            currentReviewers = *existing;
        }

        std::string newEntry = std::to_string(*reviewerIdOpt);

        std::stringstream rss(currentReviewers);
        std::string       rid;
        bool              alreadyAssigned = false;
        while (rss >> rid)
        {
            if (rid == newEntry)
            {
                alreadyAssigned = true;
                break;
            }
        }

        if (alreadyAssigned)
        {
            return osp::protocol::makeErrorResponse("ALREADY_ASSIGNED", "Reviewer " + reviewerName + " is already assigned to this paper");
        }

        currentReviewers += newEntry + "\n";
        if (!vfs_.writeFile(reviewersPath, currentReviewers))
        {
            return osp::protocol::makeErrorResponse("FS_ERROR", "Failed to save assignment");
        }

        return osp::protocol::makeSuccessResponse({
            {"message", "Reviewer assigned"},
            {"paperId", pidStr},
            {"reviewer", reviewerName},
            {"reviewerId", *reviewerIdOpt}
        });
    }

    if (cmd.name == "REVIEW")
    {
        if (!hasPermission(maybeSession->role, Permission::UploadReview))
        {
            return osp::protocol::makeErrorResponse("PERMISSION_DENIED", "Permission denied: Reviewer role required");
        }

        if (cmd.args.size() < 3)
        {
            return osp::protocol::makeErrorResponse("MISSING_ARGS", "Usage: REVIEW <PaperID> <Decision> <Comments...>\nDecisions: ACCEPT, REJECT, MINOR, MAJOR");
        }

        std::string pidStr      = cmd.args[0];
        std::string decisionStr = cmd.args[1];

        // 提取评论内容
        std::string comments;
        size_t decisionPos = cmd.rawArgs.find(decisionStr);
        if (decisionPos != std::string::npos)
        {
            size_t commentsStart = decisionPos + decisionStr.length();
            while (commentsStart < cmd.rawArgs.length()
                   && std::isspace(static_cast<unsigned char>(cmd.rawArgs[commentsStart])))
            {
                commentsStart++;
            }
            comments = cmd.rawArgs.substr(commentsStart);
        }

        if (comments.empty())
        {
            return osp::protocol::makeErrorResponse("INVALID_ARGS", "REVIEW: Comments are required");
        }

        auto decision = osp::domain::stringToReviewDecision(decisionStr);
        if (!decision)
        {
            return osp::protocol::makeErrorResponse("INVALID_ARGS", "Invalid decision. Allowed: ACCEPT, REJECT, MINOR, MAJOR");
        }

        std::string paperDir      = "/papers/" + pidStr;
        std::string reviewersPath = paperDir + "/reviewers.txt";
        auto        reviewersData = vfs_.readFile(reviewersPath);
        bool        assigned      = false;
        if (reviewersData)
        {
            std::stringstream rss(*reviewersData);
            std::string       rid;
            std::string       myIdStr = std::to_string(maybeSession->userId);
            while (rss >> rid)
            {
                if (rid == myIdStr)
                {
                    assigned = true;
                    break;
                }
            }
        }

        if (!assigned)
        {
            return osp::protocol::makeErrorResponse("PERMISSION_DENIED", "Permission denied: You are not assigned to review this paper");
        }

        std::string reviewsDir = paperDir + "/reviews";
        vfs_.createDirectory(reviewsDir);

        std::string reviewPath = reviewsDir + "/" + std::to_string(maybeSession->userId) + ".txt";

        std::ostringstream reviewContent;
        reviewContent << decisionStr << "\n" << comments;

        if (!vfs_.writeFile(reviewPath, reviewContent.str()))
        {
            return osp::protocol::makeErrorResponse("FS_ERROR", "Failed to save review");
        }

        return osp::protocol::makeSuccessResponse({
            {"message", "Review submitted successfully"},
            {"paperId", pidStr},
            {"decision", decisionStr}
        });
    }

    if (cmd.name == "LIST_REVIEWS")
    {
        if (cmd.args.empty())
        {
            return osp::protocol::makeErrorResponse("MISSING_ARGS", "Usage: LIST_REVIEWS <PaperID>");
        }

        std::string pidStr = cmd.args[0];

        std::string metaPath = "/papers/" + pidStr + "/meta.txt";
        auto        metaData = vfs_.readFile(metaPath);
        if (!metaData)
        {
            return osp::protocol::makeErrorResponse("NOT_FOUND", "Paper not found");
        }

        std::stringstream metaSS(*metaData);
        std::uint32_t     p_id;
        std::uint32_t     p_authorId;
        metaSS >> p_id >> p_authorId;

        bool isEditor = (maybeSession->role == osp::Role::Editor);
        bool isAdmin  = (maybeSession->role == osp::Role::Admin);
        bool isAuthor = (maybeSession->role == osp::Role::Author);

        if (isAuthor && p_authorId != maybeSession->userId)
        {
            return osp::protocol::makeErrorResponse("PERMISSION_DENIED", "Permission denied: You can only view reviews for your own papers");
        }

        if (!isEditor && !isAdmin && !isAuthor)
        {
            return osp::protocol::makeErrorResponse("PERMISSION_DENIED", "Permission denied");
        }

        std::string reviewsDir = "/papers/" + pidStr + "/reviews";
        auto        listing    = vfs_.listDirectory(reviewsDir);
        if (!listing)
        {
            return osp::protocol::makeSuccessResponse({{"reviews", json::array()}});
        }

        std::stringstream ss(*listing);
        std::string       entry;
        json              reviews = json::array();

        while (std::getline(ss, entry))
        {
            if (entry.empty())
                continue;
            
            std::string reviewPath = reviewsDir + "/" + entry;
            auto        reviewContent = vfs_.readFile(reviewPath);
            if (!reviewContent)
                continue;

            std::stringstream rss(*reviewContent);
            std::string       decision;
            std::string       comments;
            std::getline(rss, decision);
            std::string line;
            while (std::getline(rss, line))
            {
                comments += line + "\n";
            }
            // 去掉末尾换行
            if (!comments.empty() && comments.back() == '\n')
            {
                comments.pop_back();
            }

            std::string reviewerIdStr = entry.substr(0, entry.find('.'));

            reviews.push_back({
                {"reviewerId", reviewerIdStr},
                {"decision", decision},
                {"comments", comments}
            });
        }

        return osp::protocol::makeSuccessResponse({{"reviews", reviews}});
    }

    if (cmd.name == "DECISION")
    {
        if (!hasPermission(maybeSession->role, Permission::MakeFinalDecision))
        {
            return osp::protocol::makeErrorResponse("PERMISSION_DENIED", "Permission denied: Editor role required");
        }

        if (cmd.args.size() < 2)
        {
            return osp::protocol::makeErrorResponse("MISSING_ARGS", "Usage: DECISION <PaperID> <Decision> (ACCEPT/REJECT)");
        }

        std::string pidStr      = cmd.args[0];
        std::string decisionStr = cmd.args[1];

        if (decisionStr != "ACCEPT" && decisionStr != "REJECT")
        {
            return osp::protocol::makeErrorResponse("INVALID_ARGS", "Invalid decision. Use ACCEPT or REJECT");
        }

        std::string metaPath = "/papers/" + pidStr + "/meta.txt";
        auto        metaData = vfs_.readFile(metaPath);
        if (!metaData)
        {
            return osp::protocol::makeErrorResponse("NOT_FOUND", "Paper not found");
        }

        std::stringstream metaSS(*metaData);
        std::uint32_t     p_id;
        std::uint32_t     p_authorId;
        std::string       p_status;
        std::string       p_title;

        metaSS >> p_id >> p_authorId >> p_status;
        char dummy;
        metaSS.get(dummy);
        std::getline(metaSS, p_title);

        std::string newStatus = (decisionStr == "ACCEPT") ? "Accepted" : "Rejected";

        std::ostringstream newMeta;
        newMeta << p_id << "\n"
                << p_authorId << "\n"
                << newStatus << "\n"
                << p_title;

        if (!vfs_.writeFile(metaPath, newMeta.str()))
        {
            return osp::protocol::makeErrorResponse("FS_ERROR", "Failed to update paper status");
        }

        return osp::protocol::makeSuccessResponse({
            {"message", "Paper decision updated"},
            {"paperId", pidStr},
            {"status", newStatus}
        });
    }

    return osp::protocol::makeErrorResponse("UNKNOWN_COMMAND", "Unknown paper command: " + cmd.name);
}

std::uint32_t ServerApp::nextPaperId()
{
    std::string   path   = "/system/next_paper_id";
    std::uint32_t nextId = 1;

    vfs_.createDirectory("/system");

    auto data = vfs_.readFile(path);
    if (data)
    {
        try
        {
            nextId = std::stoul(*data);
        }
        catch (...)
        {
            nextId = 1;
        }
    }

    vfs_.writeFile(path, std::to_string(nextId + 1));

    return nextId;
}

osp::protocol::Message
ServerApp::handleFsCommand(const osp::protocol::Command&                        cmd,
                           const std::optional<osp::domain::Session>& /*maybeSession*/)
{
    using osp::protocol::Message;
    using osp::protocol::MessageType;
    using osp::protocol::json;

    if (cmd.name == "MKDIR")
    {
        if (cmd.args.empty())
        {
            return osp::protocol::makeErrorResponse("MISSING_ARGS", "MKDIR: missing path");
        }
        const std::string& path = cmd.args[0];

        bool ok = vfs_.createDirectory(path);
        if (ok)
        {
            return osp::protocol::makeSuccessResponse({{"message", "Directory created"}, {"path", path}});
        }
        return osp::protocol::makeErrorResponse("FS_ERROR", "MKDIR failed: " + path);
    }

    if (cmd.name == "WRITE")
    {
        if (cmd.rawArgs.empty())
        {
            return osp::protocol::makeErrorResponse("MISSING_ARGS", "WRITE: missing path");
        }

        std::istringstream iss(cmd.rawArgs);
        std::string        path;
        iss >> path;
        if (path.empty())
        {
            return osp::protocol::makeErrorResponse("MISSING_ARGS", "WRITE: missing path");
        }

        std::string content;
        std::getline(iss, content);
        if (!content.empty() && content.front() == ' ')
        {
            content.erase(content.begin());
        }

        bool ok = vfs_.writeFile(path, content);
        if (ok)
        {
            return osp::protocol::makeSuccessResponse({{"message", "File written"}, {"path", path}});
        }
        return osp::protocol::makeErrorResponse("FS_ERROR", "WRITE failed: " + path);
    }

    if (cmd.name == "READ")
    {
        if (cmd.args.empty())
        {
            return osp::protocol::makeErrorResponse("MISSING_ARGS", "READ: missing path");
        }
        const std::string& path = cmd.args[0];

        auto data = vfs_.readFile(path);
        if (!data)
        {
            return osp::protocol::makeErrorResponse("FS_ERROR", "READ failed: " + path);
        }
        return osp::protocol::makeSuccessResponse({{"path", path}, {"content", *data}});
    }

    if (cmd.name == "RM")
    {
        if (cmd.args.empty())
        {
            return osp::protocol::makeErrorResponse("MISSING_ARGS", "RM: missing path");
        }
        const std::string& path = cmd.args[0];

        bool ok = vfs_.removeFile(path);
        if (ok)
        {
            return osp::protocol::makeSuccessResponse({{"message", "File removed"}, {"path", path}});
        }
        return osp::protocol::makeErrorResponse("FS_ERROR", "RM failed: " + path);
    }

    if (cmd.name == "RMDIR")
    {
        if (cmd.args.empty())
        {
            return osp::protocol::makeErrorResponse("MISSING_ARGS", "RMDIR: missing path");
        }
        const std::string& path = cmd.args[0];

        bool ok = vfs_.removeDirectory(path);
        if (ok)
        {
            return osp::protocol::makeSuccessResponse({{"message", "Directory removed"}, {"path", path}});
        }
        return osp::protocol::makeErrorResponse("FS_ERROR", "RMDIR failed (maybe not empty?): " + path);
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
            return osp::protocol::makeErrorResponse("FS_ERROR", "LIST failed: " + path);
        }

        // 解析目录列表为数组
        std::stringstream ss(*listing);
        std::string       entry;
        json              entries = json::array();
        while (std::getline(ss, entry))
        {
            if (!entry.empty())
            {
                entries.push_back(entry);
            }
        }

        return osp::protocol::makeSuccessResponse({{"path", path}, {"entries", entries}});
    }

    return osp::protocol::makeErrorResponse("UNKNOWN_COMMAND", "Unknown FS command: " + cmd.name);
}

} // namespace osp::server
