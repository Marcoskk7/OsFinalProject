#include "server_app.hpp"

#include "domain/paper.hpp"
#include "domain/permissions.hpp"
#include "domain/review.hpp"

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
    auth_.addUser("author2", "author2", osp::Role::Author); // 添加第二个作者用于测试权限隔离
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
    
    // 将 LIST_PAPERS 移交给 handlePaperCommand 处理
    if (cmd.name == "LIST_PAPERS")
    {
        return handlePaperCommand(cmd, maybeSession);
    }

    // 文件系统相关命令，统一通过 handleFsCommand 处理
    if (cmd.name == "MKDIR" || cmd.name == "WRITE" || cmd.name == "READ" || cmd.name == "RM"
        || cmd.name == "RMDIR" || cmd.name == "LIST")
    {
        return handleFsCommand(cmd, maybeSession);
    }

    // 论文业务相关命令
    if (cmd.name == "SUBMIT")
    {
        return handlePaperCommand(cmd, maybeSession);
    }

    if (cmd.name == "GET_PAPER")
    {
        return handlePaperCommand(cmd, maybeSession);
    }

    if (cmd.name == "ASSIGN")
    {
        return handlePaperCommand(cmd, maybeSession);
    }

    if (cmd.name == "REVIEW")
    {
        return handlePaperCommand(cmd, maybeSession);
    }

    if (cmd.name == "LIST_REVIEWS")
    {
        return handlePaperCommand(cmd, maybeSession);
    }

    if (cmd.name == "DECISION")
    {
        return handlePaperCommand(cmd, maybeSession);
    }

    return {MessageType::Error, "Unknown command: " + cmd.name};
}

osp::protocol::Message
ServerApp::handlePaperCommand(const osp::protocol::Command&                        cmd,
                              const std::optional<osp::domain::Session>& maybeSession)
{
    using osp::protocol::Message;
    using osp::protocol::MessageType;
    using osp::domain::Permission;
    using osp::domain::hasPermission;

    if (!maybeSession)
    {
        return {MessageType::Error, "Authentication required"};
    }

    if (cmd.name == "LIST_PAPERS")
    {
        // 检查权限：Author 只能看自己的，Reviewer 看分配的，Editor/Admin 看所有
        bool isAuthor = (maybeSession->role == osp::Role::Author);
        bool isReviewer = (maybeSession->role == osp::Role::Reviewer);

        if (isAuthor && !hasPermission(maybeSession->role, Permission::ViewOwnPaperStatus))
        {
            return {MessageType::Error, "Permission denied"};
        }
        if (isReviewer && !hasPermission(maybeSession->role, Permission::DownloadAssignedPapers))
        {
            return {MessageType::Error, "Permission denied"};
        }

        // 遍历 /papers 目录
        auto listing = vfs_.listDirectory("/papers");
        if (!listing)
        {
            return {MessageType::CommandResponse, "No papers found."};
        }

        std::stringstream ss(*listing);
        std::string entry;
        std::string result;
        bool foundAny = false;

        while (std::getline(ss, entry))
        {
            // entry 格式如 "1/"
            if (entry.empty() || entry.back() != '/')
            {
                continue;
            }
            
            std::string pidStr = entry.substr(0, entry.size() - 1);
            std::string metaPath = "/papers/" + pidStr + "/meta.txt";
            
            auto metaData = vfs_.readFile(metaPath);
            if (!metaData)
            {
                continue;
            }

            // 解析 meta.txt
            // 格式: ID\nAuthorID\nStatus\nTitle
            std::stringstream metaSS(*metaData);
            std::uint32_t p_id;
            std::uint32_t p_authorId;
            std::string p_status;
            std::string p_title;

            if (!(metaSS >> p_id >> p_authorId >> p_status))
            {
                continue;
            }
            // 读取标题（可能包含空格），先跳过 status 后的换行
            char dummy;
            metaSS.get(dummy); 
            std::getline(metaSS, p_title);

            // 核心过滤逻辑：如果是 Author，必须匹配 ID
            if (isAuthor && p_authorId != maybeSession->userId)
            {
                continue;
            }

            // 核心过滤逻辑：如果是 Reviewer，必须在 reviewers.txt 中
            if (isReviewer)
            {
                std::string reviewersPath = "/papers/" + pidStr + "/reviewers.txt";
                auto reviewersData = vfs_.readFile(reviewersPath);
                bool assigned = false;
                if (reviewersData)
                {
                    std::stringstream rss(*reviewersData);
                    std::string rid;
                    std::string myIdStr = std::to_string(maybeSession->userId);
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

            if (foundAny)
            {
                result += "\n";
            }
            result += "[ID: " + std::to_string(p_id) + "] " + p_title + " (Status: " + p_status + ")";
            foundAny = true;
        }

        if (!foundAny)
        {
            return {MessageType::CommandResponse, "No papers found."};
        }

        return {MessageType::CommandResponse, result};
    }

    if (cmd.name == "GET_PAPER")
    {
        // GET_PAPER <PaperID>
        // Author 只能获取自己的论文详情
        if (cmd.args.empty())
        {
            return {MessageType::Error, "Usage: GET_PAPER <PaperID>"};
        }
        
        std::string pidStr = cmd.args[0];
        std::string metaPath = "/papers/" + pidStr + "/meta.txt";
        
        auto metaData = vfs_.readFile(metaPath);
        if (!metaData)
        {
            return {MessageType::Error, "Paper not found"};
        }

        std::stringstream metaSS(*metaData);
        std::uint32_t p_id;
        std::uint32_t p_authorId;
        std::string p_status;
        std::string p_title;

        metaSS >> p_id >> p_authorId >> p_status;
        char dummy;
        metaSS.get(dummy);
        std::getline(metaSS, p_title);

        if (maybeSession->role == osp::Role::Author && p_authorId != maybeSession->userId)
        {
            return {MessageType::Error, "Permission denied: You can only view your own papers"};
        }

        if (maybeSession->role == osp::Role::Reviewer)
        {
            // 检查是否被分配
            std::string reviewersPath = "/papers/" + pidStr + "/reviewers.txt";
            auto reviewersData = vfs_.readFile(reviewersPath);
            bool assigned = false;
            if (reviewersData)
            {
                std::stringstream rss(*reviewersData);
                std::string rid;
                std::string myIdStr = std::to_string(maybeSession->userId);
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
                return {MessageType::Error, "Permission denied: You are not assigned to this paper"};
            }
        }

        // 返回详细信息，包括内容
        std::string contentPath = "/papers/" + pidStr + "/content.txt";
        auto contentData = vfs_.readFile(contentPath);
        
        std::ostringstream oss;
        oss << "Title: " << p_title << "\n";
        oss << "Status: " << p_status << "\n";
        oss << "AuthorID: " << p_authorId << "\n";
        oss << "--------------------------------------------------\n";
        if (contentData)
        {
            oss << *contentData;
        }
        else
        {
            oss << "(No content)";
        }

        return {MessageType::CommandResponse, oss.str()};
    }

    if (cmd.name == "SUBMIT")
    {
        // SUBMIT <Title> <Content...>
        // 简单起见，Title 不能包含空格，或者我们需要更复杂的解析。
        // 这里假设 args[0] 是 Title，剩下的 rawArgs 中去除 Title 部分是 Content。

        if (!hasPermission(maybeSession->role, Permission::UploadPaper))
        {
            return {MessageType::Error, "Permission denied: Author role required"};
        }

        if (cmd.args.size() < 2)
        {
            return {MessageType::Error, "Usage: SUBMIT <Title> <Content>"};
        }

        std::string title = cmd.args[0];

        // 从 rawArgs 中提取 content
        // rawArgs: "Title Content..."
        std::string content;
        size_t titlePos = cmd.rawArgs.find(title);
        if (titlePos != std::string::npos)
        {
            size_t contentStart = titlePos + title.length();
            // skip spaces
            while (contentStart < cmd.rawArgs.length() && std::isspace(cmd.rawArgs[contentStart]))
            {
                contentStart++;
            }
            content = cmd.rawArgs.substr(contentStart);
        }

        if (content.empty())
        {
            return {MessageType::Error, "SUBMIT: Content is empty"};
        }

        // 1. 获取 Paper ID
        std::uint32_t pid = nextPaperId();

        // 2. 准备目录 /papers/<id>
        std::string paperDir = "/papers/" + std::to_string(pid);

        // 确保 /papers 存在 (忽略错误，可能已存在)
        vfs_.createDirectory("/papers");

        if (!vfs_.createDirectory(paperDir))
        {
            return {MessageType::Error, "Failed to create paper directory"};
        }

        // 3. 写入内容
        if (!vfs_.writeFile(paperDir + "/content.txt", content))
        {
            return {MessageType::Error, "Failed to save paper content"};
        }

        // 4. 写入元数据
        // 格式: ID\nAuthorID\nStatus\nTitle
        std::ostringstream meta;
        meta << pid << "\n"
             << maybeSession->userId << "\n"
             << osp::domain::paperStatusToString(osp::domain::PaperStatus::Submitted) << "\n"
             << title;

        if (!vfs_.writeFile(paperDir + "/meta.txt", meta.str()))
        {
            return {MessageType::Error, "Failed to save paper metadata"};
        }

        return {MessageType::CommandResponse, "SUBMIT success. Paper ID: " + std::to_string(pid)};
    }

    if (cmd.name == "ASSIGN")
    {
        // ASSIGN <PaperID> <ReviewerUsername>
        if (!hasPermission(maybeSession->role, Permission::AssignReviewers))
        {
            return {MessageType::Error, "Permission denied: Editor role required"};
        }

        if (cmd.args.size() < 2)
        {
            return {MessageType::Error, "Usage: ASSIGN <PaperID> <ReviewerUsername>"};
        }

        std::string pidStr = cmd.args[0];
        std::string reviewerName = cmd.args[1];

        // 1. 检查论文是否存在
        std::string paperDir = "/papers/" + pidStr;
        std::string metaPath = paperDir + "/meta.txt";
        if (!vfs_.readFile(metaPath))
        {
            return {MessageType::Error, "Paper not found: " + pidStr};
        }

        // 2. 检查 Reviewer 是否存在
        auto reviewerIdOpt = auth_.getUserId(reviewerName);
        if (!reviewerIdOpt)
        {
            return {MessageType::Error, "User not found: " + reviewerName};
        }
        // 这里可以进一步检查该用户是否真的是 Reviewer 角色，目前简化处理

        // 3. 写入分配信息 /papers/<id>/reviewers.txt
        // 格式：每行一个 ReviewerID
        std::string reviewersPath = paperDir + "/reviewers.txt";
        std::string currentReviewers;
        auto existing = vfs_.readFile(reviewersPath);
        if (existing)
        {
            currentReviewers = *existing;
        }

        std::string newEntry = std::to_string(*reviewerIdOpt);
        
        // 简单查重
        std::stringstream rss(currentReviewers);
        std::string rid;
        bool alreadyAssigned = false;
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
            return {MessageType::Error, "Reviewer " + reviewerName + " is already assigned to this paper"};
        }

        currentReviewers += newEntry + "\n";
        if (!vfs_.writeFile(reviewersPath, currentReviewers))
        {
            return {MessageType::Error, "Failed to save assignment"};
        }

        return {MessageType::CommandResponse, "Assigned " + reviewerName + " (ID: " + newEntry + ") to paper " + pidStr};
    }

    if (cmd.name == "REVIEW")
    {
        // REVIEW <PaperID> <Decision> <Comments...>
        // Decision: ACCEPT, REJECT, MINOR, MAJOR
        
        if (!hasPermission(maybeSession->role, Permission::UploadReview))
        {
            return {MessageType::Error, "Permission denied: Reviewer role required"};
        }

        if (cmd.args.size() < 3)
        {
            return {MessageType::Error, "Usage: REVIEW <PaperID> <Decision> <Comments...>\nDecisions: ACCEPT, REJECT, MINOR, MAJOR"};
        }

        std::string pidStr = cmd.args[0];
        std::string decisionStr = cmd.args[1];
        
        // 提取评论内容
        std::string comments;
        // 寻找 decisionStr 在 rawArgs 中的位置，后面即为 comments
        size_t decisionPos = cmd.rawArgs.find(decisionStr);
        if (decisionPos != std::string::npos)
        {
            size_t commentsStart = decisionPos + decisionStr.length();
            while (commentsStart < cmd.rawArgs.length() && std::isspace(cmd.rawArgs[commentsStart]))
            {
                commentsStart++;
            }
            comments = cmd.rawArgs.substr(commentsStart);
        }

        if (comments.empty())
        {
            return {MessageType::Error, "REVIEW: Comments are required"};
        }

        // 验证 Decision 格式
        auto decision = osp::domain::stringToReviewDecision(decisionStr);
        if (!decision)
        {
            return {MessageType::Error, "Invalid decision. Allowed: ACCEPT, REJECT, MINOR, MAJOR"};
        }

        // 验证是否被分配了该论文
        std::string paperDir = "/papers/" + pidStr;
        std::string reviewersPath = paperDir + "/reviewers.txt";
        auto reviewersData = vfs_.readFile(reviewersPath);
        bool assigned = false;
        if (reviewersData)
        {
            std::stringstream rss(*reviewersData);
            std::string rid;
            std::string myIdStr = std::to_string(maybeSession->userId);
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
            return {MessageType::Error, "Permission denied: You are not assigned to review this paper"};
        }

        // 存储评审
        // 路径: /papers/<pid>/reviews/<reviewer_id>.txt
        std::string reviewsDir = paperDir + "/reviews";
        vfs_.createDirectory(reviewsDir); // 确保目录存在

        std::string reviewPath = reviewsDir + "/" + std::to_string(maybeSession->userId) + ".txt";
        
        std::ostringstream reviewContent;
        reviewContent << decisionStr << "\n" << comments;

        if (!vfs_.writeFile(reviewPath, reviewContent.str()))
        {
            return {MessageType::Error, "Failed to save review"};
        }

        return {MessageType::CommandResponse, "Review submitted successfully for paper " + pidStr};
    }

    if (cmd.name == "LIST_REVIEWS")
    {
        // LIST_REVIEWS <PaperID>
        if (cmd.args.empty())
        {
            return {MessageType::Error, "Usage: LIST_REVIEWS <PaperID>"};
        }

        std::string pidStr = cmd.args[0];
        
        // 1. 鉴权：检查论文归属
        std::string metaPath = "/papers/" + pidStr + "/meta.txt";
        auto metaData = vfs_.readFile(metaPath);
        if (!metaData)
        {
            return {MessageType::Error, "Paper not found"};
        }

        std::stringstream metaSS(*metaData);
        std::uint32_t p_id;
        std::uint32_t p_authorId;
        metaSS >> p_id >> p_authorId;

        bool isEditor = (maybeSession->role == osp::Role::Editor);
        bool isAuthor = (maybeSession->role == osp::Role::Author);
        
        if (isAuthor && p_authorId != maybeSession->userId)
        {
            return {MessageType::Error, "Permission denied: You can only view reviews for your own papers"};
        }
        
        if (!isEditor && !isAuthor)
        {
             // 简单起见，Reviewer 暂时不能查看其它人的评审，或者需要更复杂的逻辑
             return {MessageType::Error, "Permission denied"};
        }

        // 2. 遍历 /papers/<id>/reviews/
        std::string reviewsDir = "/papers/" + pidStr + "/reviews";
        auto listing = vfs_.listDirectory(reviewsDir);
        if (!listing)
        {
            return {MessageType::CommandResponse, "No reviews yet."};
        }

        std::stringstream ss(*listing);
        std::string entry;
        std::ostringstream result;
        bool foundAny = false;

        while (std::getline(ss, entry))
        {
            if (entry.empty()) continue;
            // entry 可能是 "102.txt"
            std::string reviewPath = reviewsDir + "/" + entry;
            auto reviewContent = vfs_.readFile(reviewPath);
            if (!reviewContent) continue;

            // 解析评审内容
            // 格式: Decision\nComments...
            std::stringstream rss(*reviewContent);
            std::string decision;
            std::string comments;
            std::getline(rss, decision);
            // 读取剩余所有内容作为 comments
            std::string line;
            while (std::getline(rss, line))
            {
                comments += line + "\n";
            }

            // 获取 Reviewer ID (文件名即 ID.txt)
            std::string reviewerIdStr = entry.substr(0, entry.find('.'));

            result << "--------------------------------------------------\n";
            result << "Reviewer ID: " << reviewerIdStr << "\n";
            result << "Decision: " << decision << "\n";
            result << "Comments:\n" << comments << "\n";
            foundAny = true;
        }

        if (!foundAny)
        {
            return {MessageType::CommandResponse, "No reviews yet."};
        }

        return {MessageType::CommandResponse, result.str()};
    }

    if (cmd.name == "DECISION")
    {
        // DECISION <PaperID> <Decision>
        // Decision: ACCEPT, REJECT
        if (!hasPermission(maybeSession->role, Permission::MakeFinalDecision))
        {
            return {MessageType::Error, "Permission denied: Editor role required"};
        }

        if (cmd.args.size() < 2)
        {
            return {MessageType::Error, "Usage: DECISION <PaperID> <Decision> (ACCEPT/REJECT)"};
        }

        std::string pidStr = cmd.args[0];
        std::string decisionStr = cmd.args[1];

        // 简单校验
        if (decisionStr != "ACCEPT" && decisionStr != "REJECT")
        {
            return {MessageType::Error, "Invalid decision. Use ACCEPT or REJECT"};
        }

        // 1. 读取现有 Meta
        std::string metaPath = "/papers/" + pidStr + "/meta.txt";
        auto metaData = vfs_.readFile(metaPath);
        if (!metaData)
        {
            return {MessageType::Error, "Paper not found"};
        }

        std::stringstream metaSS(*metaData);
        std::uint32_t p_id;
        std::uint32_t p_authorId;
        std::string p_status;
        std::string p_title;

        metaSS >> p_id >> p_authorId >> p_status;
        char dummy;
        metaSS.get(dummy);
        std::getline(metaSS, p_title);

        // 2. 更新状态
        std::string newStatus = (decisionStr == "ACCEPT") ? "Accepted" : "Rejected";
        
        std::ostringstream newMeta;
        newMeta << p_id << "\n"
                << p_authorId << "\n"
                << newStatus << "\n"
                << p_title;

        if (!vfs_.writeFile(metaPath, newMeta.str()))
        {
            return {MessageType::Error, "Failed to update paper status"};
        }

        return {MessageType::CommandResponse, "Paper " + pidStr + " marked as " + newStatus};
    }

    return {MessageType::Error, "Unknown paper command: " + cmd.name};
}

std::uint32_t ServerApp::nextPaperId()
{
    // 简单实现：读取 /system/next_paper_id
    // 如果不存在则从 1 开始
    std::string path = "/system/next_paper_id";
    std::uint32_t nextId = 1;

    // 确保存储系统数据的目录存在
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

    // 更新回文件
    vfs_.writeFile(path, std::to_string(nextId + 1));

    return nextId;
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


