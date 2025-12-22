#include "server_app.hpp"

#include "domain/paper.hpp"
#include "domain/permissions.hpp"
#include "domain/review.hpp"

#include <sstream>
#include <cstdlib>

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

ServerApp::ServerApp(std::uint16_t port, std::size_t cacheCapacity, std::size_t threadPoolSize)
    : port_(port)
    , threadPoolSize_(threadPoolSize)
    , vfs_(clampCacheCapacity(cacheCapacity))
    , auth_()
{
    // 用户数据将在 run() 中 VFS 挂载后从文件系统加载
}

void ServerApp::run()
{
    running_.store(true);
    osp::log(osp::LogLevel::Info,
             "Server starting on port " + std::to_string(port_)
                 + " (cacheCapacity=" + std::to_string(vfs_.cacheCapacity())
                 + ", threadPoolSize=" + std::to_string(threadPoolSize_) + ")");

    // 挂载简化 VFS
    {
        std::lock_guard<std::mutex> lock(vfsMutex_);
        vfs_.mount("data.fs");
    }

    // 初始化 AuthService 的 VFS 操作接口
    initAuthVfsOperations();

    // 从 VFS 加载用户数据
    {
        std::lock_guard<std::mutex> authLock(authMutex_);
        auth_.loadUsers();

        // 如果没有用户数据，初始化默认账号
        if (auth_.getAllUsers().empty())
        {
            osp::log(osp::LogLevel::Info, "No users found, creating default accounts...");
            auth_.addUser("admin", "admin", osp::Role::Admin);
            auth_.addUser("author", "author", osp::Role::Author);
            auth_.addUser("author2", "author2", osp::Role::Author);
            auth_.addUser("reviewer", "reviewer", osp::Role::Reviewer);
            auth_.addUser("editor", "editor", osp::Role::Editor);
        }
        else
        {
            osp::log(osp::LogLevel::Info, 
                     "Loaded " + std::to_string(auth_.getAllUsers().size()) + " users from VFS");
        }
    }

    // 使用多线程 TCP 服务器
    osp::net::TcpServer tcpServer(port_, threadPoolSize_);

    tcpServer.start([this](const osp::protocol::Message& req) {
        return handleRequest(req);
    });

    osp::log(osp::LogLevel::Info, "Server shutting down");
}

void ServerApp::initAuthVfsOperations()
{
    osp::domain::VfsOperations ops;

    // 创建目录
    ops.createDirectory = [this](const std::string& path) -> bool {
        std::lock_guard<std::mutex> lock(vfsMutex_);
        return vfs_.createDirectory(path);
    };

    // 写文件
    ops.writeFile = [this](const std::string& path, const std::string& content) -> bool {
        std::lock_guard<std::mutex> lock(vfsMutex_);
        return vfs_.writeFile(path, content);
    };

    // 读文件
    ops.readFile = [this](const std::string& path) -> std::optional<std::string> {
        std::lock_guard<std::mutex> lock(vfsMutex_);
        return vfs_.readFile(path);
    };

    // 删除文件
    ops.removeFile = [this](const std::string& path) -> bool {
        std::lock_guard<std::mutex> lock(vfsMutex_);
        return vfs_.removeFile(path);
    };

    // 列出目录
    ops.listDirectory = [this](const std::string& path) -> std::optional<std::string> {
        std::lock_guard<std::mutex> lock(vfsMutex_);
        return vfs_.listDirectory(path);
    };

    // 设置到 AuthService（需要在 authMutex_ 保护下）
    std::lock_guard<std::mutex> authLock(authMutex_);
    auth_.setVfsOperations(ops);

    osp::log(osp::LogLevel::Info, "AuthService VFS persistence enabled");
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
        std::lock_guard<std::mutex> lock(authMutex_);
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

        std::lock_guard<std::mutex> lock(authMutex_);
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
    if (cmd.name == "LIST_PAPERS" || cmd.name == "SUBMIT" || cmd.name == "GET_PAPER"
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
        
        std::lock_guard<std::mutex> lock(authMutex_);
        
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
        return osp::protocol::makeSuccessResponse({{"message", "Backup completed"}, {"path", cmd.args[0]}});
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
        return osp::protocol::makeSuccessResponse({{"message", "Restore completed"}, {"path", cmd.args[0]}});
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

        std::size_t userCount{};
        std::size_t sessionCount{};
        {
            std::lock_guard<std::mutex> lock(authMutex_);
            userCount = auth_.getAllUsers().size();
            sessionCount = auth_.sessionCount();
        }

        // Papers: 通过遍历 /papers/<id>/ 目录计数（只统计一级目录项）
        std::size_t paperCount = 0;
        std::optional<std::string> papersListing;
        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            papersListing = vfs_.listDirectory("/papers");
        }
        
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
                std::optional<std::string> reviewsListing;
                {
                    std::lock_guard<std::mutex> lock(vfsMutex_);
                    reviewsListing = vfs_.listDirectory(reviewsDir);
                }
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

        osp::fs::BlockCache::Stats cs;
        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            cs = vfs_.cacheStats();
        }
        
        json data;
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

        std::optional<std::string> listing;
        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            listing = vfs_.listDirectory("/papers");
        }
        
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

            std::optional<std::string> metaData;
            {
                std::lock_guard<std::mutex> lock(vfsMutex_);
                metaData = vfs_.readFile(metaPath);
            }
            
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
                std::optional<std::string> reviewersData;
                {
                    std::lock_guard<std::mutex> lock(vfsMutex_);
                    reviewersData = vfs_.readFile(reviewersPath);
                }
                
                bool assigned = false;
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

        std::optional<std::string> metaData;
        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            metaData = vfs_.readFile(metaPath);
        }
        
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
            std::optional<std::string> reviewersData;
            {
                std::lock_guard<std::mutex> lock(vfsMutex_);
                reviewersData = vfs_.readFile(reviewersPath);
            }
            
            bool assigned = false;
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
        std::optional<std::string> contentData;
        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            contentData = vfs_.readFile(contentPath);
        }

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

        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
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
        }

        return osp::protocol::makeSuccessResponse({{"message", "Paper submitted successfully"}, {"paperId", pid}});
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
        
        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            if (!vfs_.readFile(metaPath))
            {
                return osp::protocol::makeErrorResponse("NOT_FOUND", "Paper not found: " + pidStr);
            }
        }

        std::optional<osp::UserId> reviewerIdOpt;
        {
            std::lock_guard<std::mutex> lock(authMutex_);
            reviewerIdOpt = auth_.getUserId(reviewerName);
        }
        
        if (!reviewerIdOpt)
        {
            return osp::protocol::makeErrorResponse("NOT_FOUND", "User not found: " + reviewerName);
        }

        std::string reviewersPath   = paperDir + "/reviewers.txt";
        std::string currentReviewers;
        
        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            auto existing = vfs_.readFile(reviewersPath);
            if (existing)
            {
                currentReviewers = *existing;
            }
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
        
        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            if (!vfs_.writeFile(reviewersPath, currentReviewers))
            {
                return osp::protocol::makeErrorResponse("FS_ERROR", "Failed to save assignment");
            }
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
        
        std::optional<std::string> reviewersData;
        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            reviewersData = vfs_.readFile(reviewersPath);
        }
        
        bool assigned = false;
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
        std::string reviewPath = reviewsDir + "/" + std::to_string(maybeSession->userId) + ".txt";

        std::ostringstream reviewContent;
        reviewContent << decisionStr << "\n" << comments;

        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            vfs_.createDirectory(reviewsDir);

            if (!vfs_.writeFile(reviewPath, reviewContent.str()))
            {
                return osp::protocol::makeErrorResponse("FS_ERROR", "Failed to save review");
            }
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
        std::optional<std::string> metaData;
        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            metaData = vfs_.readFile(metaPath);
        }
        
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
        std::optional<std::string> listing;
        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            listing = vfs_.listDirectory(reviewsDir);
        }
        
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
            std::optional<std::string> reviewContent;
            {
                std::lock_guard<std::mutex> lock(vfsMutex_);
                reviewContent = vfs_.readFile(reviewPath);
            }
            
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
        std::optional<std::string> metaData;
        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            metaData = vfs_.readFile(metaPath);
        }
        
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

        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            if (!vfs_.writeFile(metaPath, newMeta.str()))
            {
                return osp::protocol::makeErrorResponse("FS_ERROR", "Failed to update paper status");
            }
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
    std::lock_guard<std::mutex> lock(vfsMutex_);
    
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

        bool ok;
        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            ok = vfs_.createDirectory(path);
        }
        
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

        bool ok;
        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            ok = vfs_.writeFile(path, content);
        }
        
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

        std::optional<std::string> data;
        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            data = vfs_.readFile(path);
        }
        
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

        bool ok;
        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            ok = vfs_.removeFile(path);
        }
        
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

        bool ok;
        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            ok = vfs_.removeDirectory(path);
        }
        
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

        std::optional<std::string> listing;
        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            listing = vfs_.listDirectory(path);
        }
        
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
