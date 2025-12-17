#include "cli.hpp"

#include "client/net/tcp_client.hpp"
#include "common/logger.hpp"

#include <cctype>
#include <iostream>
#include <string>
#include <string_view>

namespace osp::client
{

namespace
{

bool isLoginCommand(std::string_view line)
{
    std::size_t i = 0;
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])))
    {
        ++i;
    }
    if (i + 5 > line.size())
    {
        return false;
    }

    const char* kw = "LOGIN";
    for (std::size_t k = 0; k < 5; ++k)
    {
        if (std::toupper(static_cast<unsigned char>(line[i + k])) != kw[k])
        {
            return false;
        }
    }

    if (i + 5 < line.size())
    {
        char c = line[i + 5];
        if (!std::isspace(static_cast<unsigned char>(c)))
        {
            return false;
        }
    }
    return true;
}

bool isCdCommand(std::string_view line)
{
    std::size_t i = 0;
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])))
    {
        ++i;
    }
    if (i + 2 > line.size())
    {
        return false;
    }

    const char* kw = "CD";
    for (std::size_t k = 0; k < 2; ++k)
    {
        if (std::toupper(static_cast<unsigned char>(line[i + k])) != kw[k])
        {
            return false;
        }
    }

    if (i + 2 < line.size())
    {
        char c = line[i + 2];
        if (!std::isspace(static_cast<unsigned char>(c)))
        {
            return false;
        }
    }
    return true;
}

} // namespace

osp::protocol::json Cli::buildJsonPayload(const std::string& line) const
{
    using osp::protocol::json;
    using osp::protocol::Command;

    // 解析命令行
    Command cmd = osp::protocol::parseCommandLine(line);
    
    // 对于 LIST 命令，如果没有参数则使用当前目录
    if (cmd.name == "LIST" && cmd.rawArgs.empty())
    {
        cmd.rawArgs = currentPath_;
        cmd.args.clear();
        cmd.args.push_back(currentPath_);
    }

    // 如果已登录且不是 LOGIN 命令，携带 sessionId
    if (!sessionId_.empty() && !isLoginCommand(line))
    {
        cmd.sessionId = sessionId_;
    }

    return osp::protocol::commandToJson(cmd);
}

std::optional<osp::protocol::Message> Cli::sendRequest(const osp::protocol::json& payload)
{
    osp::protocol::Message req;
    req.type = osp::protocol::MessageType::CommandRequest;
    req.payload = payload;

    osp::log(osp::LogLevel::Info, "Send request: " + payload.dump() + " to " + host_ + ":" + std::to_string(port_));

    osp::net::TcpClient tcpClient(host_, port_);
    return tcpClient.request(req);
}

void Cli::handleLoginResponse(const osp::protocol::Message& resp)
{
    using osp::protocol::json;

    if (resp.type != osp::protocol::MessageType::CommandResponse)
    {
        return;
    }

    // 检查是否成功
    if (!resp.payload.value("ok", false))
    {
        return;
    }

    const json& data = resp.payload.value("data", json::object());
    
    sessionId_ = data.value("sessionId", "");
    currentUser_ = data.value("username", "");
    currentRole_ = data.value("role", "");

    if (!sessionId_.empty())
    {
        osp::log(osp::LogLevel::Info, "Logged in as " + currentUser_ + " (" + currentRole_ + ")");
    }
}

void Cli::printResponse(const osp::protocol::Message& resp) const
{
    using osp::protocol::json;

    // 输出格式化的 JSON
    std::cout << resp.payload.dump(2) << '\n';
}

void Cli::run()
{
    osp::log(osp::LogLevel::Info, "Client CLI started. Type commands or 'quit' to exit.");
    printGeneralGuide();

    for (;;)
    {
        std::cout << "> " << std::flush;
        std::string line;
        if (!std::getline(std::cin, line))
        {
            break;
        }

        // 本地帮助命令
        if (line == "ROLE_HELP" || line == "role_help")
        {
            printRoleGuide();
            continue;
        }

        if (line == "quit" || line == "exit" || line == "q" || line == "Q")
        {
            osp::log(osp::LogLevel::Info, "Client exiting by user command");
            break;
        }

        if (line.empty())
        {
            continue;
        }

        // 角色数字菜单处理
        if (currentRole_ == "Author")
        {
            if (handleAuthorMenuInput(line))
            {
                continue;
            }
        }
        else if (currentRole_ == "Reviewer")
        {
            if (handleReviewerMenuInput(line))
            {
                continue;
            }
        }
        else if (currentRole_ == "Admin")
        {
            if (handleAdminMenuInput(line))
            {
                continue;
            }
        }
        else if (currentRole_ == "Editor")
        {
            if (handleEditorMenuInput(line))
            {
                continue;
            }
        }

        // CD 命令处理
        if (isCdCommand(line))
        {
            osp::protocol::Command cmd = osp::protocol::parseCommandLine(line);
            if (cmd.args.empty())
            {
                std::cout << "CD: missing path\n";
                continue;
            }

            const std::string& target = cmd.args[0];

            std::string newPath;
            if (!target.empty() && target[0] == '/')
            {
                newPath = target;
            }
            else
            {
                if (currentPath_ == "/" || currentPath_.empty())
                {
                    newPath = "/" + target;
                }
                else
                {
                    newPath = currentPath_ + "/" + target;
                }
            }

            // 验证目录存在
            osp::protocol::json listPayload;
            listPayload["cmd"] = "LIST";
            listPayload["args"] = {newPath};
            if (!sessionId_.empty())
            {
                listPayload["sessionId"] = sessionId_;
            }
            else
            {
                listPayload["sessionId"] = nullptr;
            }

            auto resp = sendRequest(listPayload);
            if (!resp)
            {
                osp::log(osp::LogLevel::Error, "CD: failed to contact server");
                std::cout << "CD: failed to contact server\n";
                continue;
            }

            if (resp->type == osp::protocol::MessageType::Error || !resp->payload.value("ok", false))
            {
                std::cout << "CD failed: " << resp->payload.dump() << '\n';
                continue;
            }

            currentPath_ = newPath;
            std::cout << "Current directory: " << currentPath_ << '\n';
            continue;
        }

        // 构建 JSON 负载
        osp::protocol::json payload = buildJsonPayload(line);

        // 发送请求
        auto resp = sendRequest(payload);
        if (!resp)
        {
            osp::log(osp::LogLevel::Error, "Failed to get response from server");
            continue;
        }

        bool justLoggedIn = false;
        // 处理登录响应
        if (isLoginCommand(line))
        {
            std::string oldSessionId = sessionId_;
            handleLoginResponse(*resp);
            if (sessionId_ != oldSessionId && !sessionId_.empty())
            {
                justLoggedIn = true;
            }
        }

        // 打印响应
        printResponse(*resp);

        if (justLoggedIn)
        {
            std::cout << "Logged in as " << currentUser_ << " (" << currentRole_ << "). Type ROLE_HELP to see available commands.\n";
        }
    }
}

void Cli::printGeneralGuide() const
{
    std::cout << "=== 指引 ===\n";
    std::cout << "基础命令:\n";
    std::cout << "  PING                      - 连通性测试\n";
    std::cout << "  LOGIN <user> <pass>       - 登录\n";
    std::cout << "  ROLE_HELP                 - 查看当前角色可用命令/菜单\n";
    std::cout << "  quit / exit / q           - 退出客户端\n";
    std::cout << "文件系统命令:\n";
    std::cout << "  LIST [path] | MKDIR <path> | WRITE <path> <content> | READ <path> | RM <path> | RMDIR <path> | CD <path>\n";
    std::cout << "内置账号（用户名=密码）：admin / author / reviewer / editor\n";
    std::cout << "----------------\n";
}

void Cli::printRoleGuide() const
{
    if (currentRole_ == "Author")
    {
        printAuthorNumericMenu();
    }
    else if (currentRole_ == "Reviewer")
    {
        printReviewerNumericMenu();
    }
    else if (currentRole_ == "Admin")
    {
        printAdminNumericMenu();
    }
    else if (currentRole_ == "Editor")
    {
        printEditorNumericMenu();
    }
    else
    {
        std::cout << "未知角色: " << currentRole_ << '\n';
    }
}

void Cli::printAuthorNumericMenu() const
{
    std::cout << "[Author 数字菜单]\n";
    std::cout << "  1) 提交新论文 (SUBMIT)\n";
    std::cout << "  2) 查看我的论文列表 (LIST_PAPERS)\n";
    std::cout << "  3) 查看论文详情 (GET_PAPER)\n";
    std::cout << "  4) 查看评审意见/状态 (LIST_REVIEWS)\n";
    std::cout << "  (直接输入数字开始操作；也可以直接输入原始命令)\n";
    std::cout << "----------------\n";
}

bool Cli::handleAuthorMenuInput(const std::string& line)
{
    auto trim = [](const std::string& s) {
        std::size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return std::string{};
        std::size_t e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    };

    const std::string t = trim(line);

    if (authorWizard_ != AuthorWizard::None)
    {
        switch (authorWizard_)
        {
        case AuthorWizard::SubmitAskTitle:
            tempTitle_ = t;
            std::cout << "输入论文内容（可包含空格）: ";
            authorWizard_ = AuthorWizard::SubmitAskContent;
            return true;
        case AuthorWizard::SubmitAskContent:
        {
            const std::string content = t;
            if (tempTitle_.empty())
            {
                std::cout << "标题不能为空。重新输入标题（不要含空格）: ";
                authorWizard_ = AuthorWizard::SubmitAskTitle;
                return true;
            }
            if (content.empty())
            {
                std::cout << "内容不能为空。重新输入论文内容: ";
                return true;
            }

            auto payload = buildJsonPayload("SUBMIT " + tempTitle_ + " " + content);
            if (auto resp = sendRequest(payload))
            {
                printResponse(*resp);
            }
            else
            {
                std::cout << "发送失败\n";
            }
            std::cout << "输入 c 继续提交，m 返回作者菜单，其他退出向导: ";
            authorWizard_ = AuthorWizard::PostSubmitPrompt;
            return true;
        }
        case AuthorWizard::ViewAskPaperId:
        {
            auto payload = buildJsonPayload("GET_PAPER " + t);
            if (auto resp = sendRequest(payload))
            {
                printResponse(*resp);
            }
            else
            {
                std::cout << "发送失败\n";
            }
            std::cout << "输入 c 继续查看详情，m 返回作者菜单，其他退出向导: ";
            authorWizard_ = AuthorWizard::PostViewPrompt;
            return true;
        }
        case AuthorWizard::ViewReviewsAskPaperId:
        {
            auto payload = buildJsonPayload("LIST_REVIEWS " + t);
            if (auto resp = sendRequest(payload))
            {
                printResponse(*resp);
            }
            else
            {
                std::cout << "发送失败\n";
            }
            std::cout << "输入 c 继续查看评审，m 返回作者菜单，其他退出向导: ";
            authorWizard_ = AuthorWizard::PostViewReviewsPrompt;
            return true;
        }
        case AuthorWizard::PostSubmitPrompt:
            if (t == "c" || t == "C")
            {
                authorWizard_ = AuthorWizard::SubmitAskTitle;
                std::cout << "提交新论文，输入标题（不要含空格）: ";
                return true;
            }
            if (t == "m" || t == "M")
            {
                authorWizard_ = AuthorWizard::None;
                printAuthorNumericMenu();
                return true;
            }
            authorWizard_ = AuthorWizard::None;
            return true;
        case AuthorWizard::PostViewPrompt:
            if (t == "c" || t == "C")
            {
                authorWizard_ = AuthorWizard::ViewAskPaperId;
                std::cout << "查看论文详情，输入 paper_id: ";
                return true;
            }
            if (t == "m" || t == "M")
            {
                authorWizard_ = AuthorWizard::None;
                printAuthorNumericMenu();
                return true;
            }
            authorWizard_ = AuthorWizard::None;
            return true;
        case AuthorWizard::PostViewReviewsPrompt:
            if (t == "c" || t == "C")
            {
                authorWizard_ = AuthorWizard::ViewReviewsAskPaperId;
                std::cout << "查看评审意见/状态，输入 paper_id: ";
                return true;
            }
            if (t == "m" || t == "M")
            {
                authorWizard_ = AuthorWizard::None;
                printAuthorNumericMenu();
                return true;
            }
            authorWizard_ = AuthorWizard::None;
            return true;
        default: break;
        }
    }

    if (t == "1")
    {
        authorWizard_ = AuthorWizard::SubmitAskTitle;
        std::cout << "提交新论文，输入标题（不要含空格）: ";
        return true;
    }
    if (t == "2")
    {
        auto payload = buildJsonPayload("LIST_PAPERS");
        if (auto resp = sendRequest(payload))
        {
            printResponse(*resp);
        }
        else
        {
            std::cout << "发送失败\n";
        }
        // 打印菜单，方便用户继续操作
        printAuthorNumericMenu();
        return true;
    }
    if (t == "3")
    {
        authorWizard_ = AuthorWizard::ViewAskPaperId;
        std::cout << "查看论文详情，输入 paper_id: ";
        return true;
    }
    if (t == "4")
    {
        authorWizard_ = AuthorWizard::ViewReviewsAskPaperId;
        std::cout << "查看评审意见/状态，输入 paper_id: ";
        return true;
    }

    return false;
}

void Cli::printReviewerNumericMenu() const
{
    std::cout << "[Reviewer 数字菜单]\n";
    std::cout << "  1) 查看分配给我的论文列表 (LIST_PAPERS)\n";
    std::cout << "  2) 查看论文详情 (GET_PAPER)\n";
    std::cout << "  3) 提交评审报告 (REVIEW)\n";
    std::cout << "  (直接输入数字开始操作；也可以直接输入原始命令)\n";
    std::cout << "----------------\n";
}

bool Cli::handleReviewerMenuInput(const std::string& line)
{
    auto trim = [](const std::string& s) {
        std::size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return std::string{};
        std::size_t e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    };

    const std::string t = trim(line);

    if (reviewerWizard_ != ReviewerWizard::None)
    {
        switch (reviewerWizard_)
        {
        case ReviewerWizard::ViewAskPaperId:
        {
            auto payload = buildJsonPayload("GET_PAPER " + t);
            if (auto resp = sendRequest(payload))
            {
                printResponse(*resp);
            }
            else
            {
                std::cout << "发送失败\n";
            }
            std::cout << "输入 c 继续查看详情，m 返回审稿人菜单，其他退出向导: ";
            reviewerWizard_ = ReviewerWizard::PostViewPrompt;
            return true;
        }
        case ReviewerWizard::ReviewAskPaperId:
            tempPaperId_ = t;
            std::cout << "输入决定（1: ACCEPT, 2: REJECT, 3: MINOR, 4: MAJOR）: ";
            reviewerWizard_ = ReviewerWizard::ReviewAskDecision;
            return true;
        case ReviewerWizard::ReviewAskDecision:
        {
            std::string decision;
            if (t == "1") decision = "ACCEPT";
            else if (t == "2") decision = "REJECT";
            else if (t == "3") decision = "MINOR";
            else if (t == "4") decision = "MAJOR";
            else decision = t; // 支持直接输入
            tempDecision_ = decision;
            std::cout << "输入评审意见（可包含空格，必填）: ";
            reviewerWizard_ = ReviewerWizard::ReviewAskComments;
            return true;
        }
        case ReviewerWizard::ReviewAskComments:
        {
            const std::string comments = t;
            auto payload = buildJsonPayload("REVIEW " + tempPaperId_ + " " + tempDecision_ + " " + comments);
            if (auto resp = sendRequest(payload))
            {
                printResponse(*resp);
            }
            else
            {
                std::cout << "发送失败\n";
            }
            std::cout << "输入 c 继续评审，m 返回审稿人菜单，其他退出向导: ";
            reviewerWizard_ = ReviewerWizard::PostReviewPrompt;
            return true;
        }
        case ReviewerWizard::PostViewPrompt:
            if (t == "c" || t == "C")
            {
                reviewerWizard_ = ReviewerWizard::ViewAskPaperId;
                std::cout << "查看论文详情，输入 paper_id: ";
                return true;
            }
            if (t == "m" || t == "M")
            {
                reviewerWizard_ = ReviewerWizard::None;
                printReviewerNumericMenu();
                return true;
            }
            reviewerWizard_ = ReviewerWizard::None;
            return true;
        case ReviewerWizard::PostReviewPrompt:
            if (t == "c" || t == "C")
            {
                reviewerWizard_ = ReviewerWizard::ReviewAskPaperId;
                std::cout << "提交评审，输入 paper_id: ";
                return true;
            }
            if (t == "m" || t == "M")
            {
                reviewerWizard_ = ReviewerWizard::None;
                printReviewerNumericMenu();
                return true;
            }
            reviewerWizard_ = ReviewerWizard::None;
            return true;
        default: break;
        }
    }

    if (t == "1")
    {
        auto payload = buildJsonPayload("LIST_PAPERS");
        if (auto resp = sendRequest(payload))
        {
            printResponse(*resp);
        }
        else
        {
            std::cout << "发送失败\n";
        }
        printReviewerNumericMenu();
        return true;
    }
    if (t == "2")
    {
        reviewerWizard_ = ReviewerWizard::ViewAskPaperId;
        std::cout << "查看论文详情，输入 paper_id: ";
        return true;
    }
    if (t == "3")
    {
        reviewerWizard_ = ReviewerWizard::ReviewAskPaperId;
        std::cout << "提交评审，输入 paper_id: ";
        return true;
    }

    return false;
}

void Cli::printAdminNumericMenu() const
{
    std::cout << "[Admin 数字菜单]\n";
    std::cout << "  1) 列出用户 (MANAGE_USERS LIST)\n";
    std::cout << "  2) 添加 Reviewer (MANAGE_USERS ADD)\n";
    std::cout << "  3) 删除用户 (MANAGE_USERS REMOVE)\n";
    std::cout << "  4) 更新用户角色 (MANAGE_USERS UPDATE_ROLE)\n";
    std::cout << "  5) 重置用户密码 (MANAGE_USERS RESET_PASSWORD)\n";
    std::cout << "  6) 备份 (BACKUP)\n";
    std::cout << "  7) 恢复 (RESTORE)\n";
    std::cout << "  8) 查看系统状态 (VIEW_SYSTEM_STATUS)\n";
    std::cout << "  (直接输入数字开始操作；也可以直接输入原始命令)\n";
    std::cout << "----------------\n";
}

bool Cli::handleAdminMenuInput(const std::string& line)
{
    auto trim = [](const std::string& s) {
        std::size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return std::string{};
        std::size_t e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    };

    const std::string t = trim(line);

    if (adminWizard_ != AdminWizard::None)
    {
        switch (adminWizard_)
        {
        case AdminWizard::AddReviewerAskName:
            tempUsername_ = t;
            std::cout << "输入密码: ";
            adminWizard_ = AdminWizard::AddReviewerAskPassword;
            return true;
        case AdminWizard::AddReviewerAskPassword:
        {
            tempPassword_ = t.empty() ? "123456" : t;
            auto payload = buildJsonPayload("MANAGE_USERS ADD " + tempUsername_ + " " + tempPassword_ + " Reviewer");
            if (auto resp = sendRequest(payload))
            {
                printResponse(*resp);
            }
            else
            {
                std::cout << "发送失败\n";
            }
            std::cout << "输入 c 继续添加 Reviewer，m 返回管理员菜单，其他退出向导: ";
            adminWizard_ = AdminWizard::PostAddPrompt;
            return true;
        }
        case AdminWizard::RemoveUserAskName:
        {
            auto payload = buildJsonPayload("MANAGE_USERS REMOVE " + t);
            if (auto resp = sendRequest(payload))
            {
                printResponse(*resp);
            }
            else
            {
                std::cout << "发送失败\n";
            }
            std::cout << "输入 c 继续删除，m 返回管理员菜单，其他退出向导: ";
            adminWizard_ = AdminWizard::PostRemovePrompt;
            return true;
        }
        case AdminWizard::UpdateRoleAskName:
            tempUsername_ = t;
            std::cout << "输入角色（Author/Reviewer/Editor/Admin）: ";
            adminWizard_ = AdminWizard::UpdateRoleAskRole;
            return true;
        case AdminWizard::UpdateRoleAskRole:
        {
            tempRole_ = t;
            auto payload = buildJsonPayload("MANAGE_USERS UPDATE_ROLE " + tempUsername_ + " " + tempRole_);
            if (auto resp = sendRequest(payload))
            {
                printResponse(*resp);
            }
            else
            {
                std::cout << "发送失败\n";
            }
            std::cout << "输入 c 继续更新角色，m 返回管理员菜单，其他退出向导: ";
            adminWizard_ = AdminWizard::PostUpdatePrompt;
            return true;
        }
        case AdminWizard::ResetPwdAskName:
            tempUsername_ = t;
            std::cout << "输入新密码: ";
            adminWizard_ = AdminWizard::ResetPwdAskNewPwd;
            return true;
        case AdminWizard::ResetPwdAskNewPwd:
        {
            tempPassword_ = t;
            auto payload = buildJsonPayload("MANAGE_USERS RESET_PASSWORD " + tempUsername_ + " " + tempPassword_);
            if (auto resp = sendRequest(payload))
            {
                printResponse(*resp);
            }
            else
            {
                std::cout << "发送失败\n";
            }
            std::cout << "输入 c 继续重置密码，m 返回管理员菜单，其他退出向导: ";
            adminWizard_ = AdminWizard::PostResetPwdPrompt;
            return true;
        }
        case AdminWizard::BackupAskPath:
        {
            auto payload = buildJsonPayload("BACKUP " + t);
            if (auto resp = sendRequest(payload))
            {
                printResponse(*resp);
            }
            else
            {
                std::cout << "发送失败\n";
            }
            std::cout << "输入 c 继续备份，m 返回管理员菜单，其他退出向导: ";
            adminWizard_ = AdminWizard::PostBackupPrompt;
            return true;
        }
        case AdminWizard::RestoreAskPath:
        {
            auto payload = buildJsonPayload("RESTORE " + t);
            if (auto resp = sendRequest(payload))
            {
                printResponse(*resp);
            }
            else
            {
                std::cout << "发送失败\n";
            }
            std::cout << "输入 c 继续恢复，m 返回管理员菜单，其他退出向导: ";
            adminWizard_ = AdminWizard::PostRestorePrompt;
            return true;
        }
        case AdminWizard::PostAddPrompt:
            if (t == "c" || t == "C")
            {
                adminWizard_ = AdminWizard::AddReviewerAskName;
                std::cout << "添加 Reviewer，输入用户名: ";
                return true;
            }
            if (t == "m" || t == "M")
            {
                adminWizard_ = AdminWizard::None;
                printAdminNumericMenu();
                return true;
            }
            adminWizard_ = AdminWizard::None;
            return true;
        case AdminWizard::PostRemovePrompt:
            if (t == "c" || t == "C")
            {
                adminWizard_ = AdminWizard::RemoveUserAskName;
                std::cout << "删除用户，输入用户名: ";
                return true;
            }
            if (t == "m" || t == "M")
            {
                adminWizard_ = AdminWizard::None;
                printAdminNumericMenu();
                return true;
            }
            adminWizard_ = AdminWizard::None;
            return true;
        case AdminWizard::PostUpdatePrompt:
            if (t == "c" || t == "C")
            {
                adminWizard_ = AdminWizard::UpdateRoleAskName;
                std::cout << "更新角色，输入用户名: ";
                return true;
            }
            if (t == "m" || t == "M")
            {
                adminWizard_ = AdminWizard::None;
                printAdminNumericMenu();
                return true;
            }
            adminWizard_ = AdminWizard::None;
            return true;
        case AdminWizard::PostResetPwdPrompt:
            if (t == "c" || t == "C")
            {
                adminWizard_ = AdminWizard::ResetPwdAskName;
                std::cout << "重置密码，输入用户名: ";
                return true;
            }
            if (t == "m" || t == "M")
            {
                adminWizard_ = AdminWizard::None;
                printAdminNumericMenu();
                return true;
            }
            adminWizard_ = AdminWizard::None;
            return true;
        case AdminWizard::PostBackupPrompt:
            if (t == "c" || t == "C")
            {
                adminWizard_ = AdminWizard::BackupAskPath;
                std::cout << "备份路径: ";
                return true;
            }
            if (t == "m" || t == "M")
            {
                adminWizard_ = AdminWizard::None;
                printAdminNumericMenu();
                return true;
            }
            adminWizard_ = AdminWizard::None;
            return true;
        case AdminWizard::PostRestorePrompt:
            if (t == "c" || t == "C")
            {
                adminWizard_ = AdminWizard::RestoreAskPath;
                std::cout << "恢复路径: ";
                return true;
            }
            if (t == "m" || t == "M")
            {
                adminWizard_ = AdminWizard::None;
                printAdminNumericMenu();
                return true;
            }
            adminWizard_ = AdminWizard::None;
            return true;
        default: break;
        }
    }

    if (t == "1")
    {
        auto payload = buildJsonPayload("MANAGE_USERS LIST");
        if (auto resp = sendRequest(payload))
        {
            printResponse(*resp);
        }
        else
        {
            std::cout << "发送失败\n";
        }
        return true;
    }
    if (t == "2")
    {
        adminWizard_ = AdminWizard::AddReviewerAskName;
        std::cout << "添加 Reviewer，输入用户名: ";
        return true;
    }
    if (t == "3")
    {
        adminWizard_ = AdminWizard::RemoveUserAskName;
        std::cout << "删除用户，输入用户名: ";
        return true;
    }
    if (t == "4")
    {
        adminWizard_ = AdminWizard::UpdateRoleAskName;
        std::cout << "更新角色，输入用户名: ";
        return true;
    }
    if (t == "5")
    {
        adminWizard_ = AdminWizard::ResetPwdAskName;
        std::cout << "重置密码，输入用户名: ";
        return true;
    }
    if (t == "6")
    {
        adminWizard_ = AdminWizard::BackupAskPath;
        std::cout << "备份路径: ";
        return true;
    }
    if (t == "7")
    {
        adminWizard_ = AdminWizard::RestoreAskPath;
        std::cout << "恢复路径: ";
        return true;
    }
    if (t == "8")
    {
        auto payload = buildJsonPayload("VIEW_SYSTEM_STATUS");
        if (auto resp = sendRequest(payload))
        {
            printResponse(*resp);
        }
        else
        {
            std::cout << "发送失败\n";
        }
        return true;
    }

    return false;
}

void Cli::printEditorNumericMenu() const
{
    std::cout << "[Editor 数字菜单]\n";
    std::cout << "  1) 查看所有论文 (LIST_PAPERS)\n";
    std::cout << "  2) 查看论文详情 (GET_PAPER)\n";
    std::cout << "  3) 指派审稿人 (ASSIGN)\n";
    std::cout << "  4) 查看论文所有评审 (LIST_REVIEWS)\n";
    std::cout << "  5) 最终决定 (DECISION)\n";
    std::cout << "  (直接输入数字开始操作；也可以直接输入原始命令)\n";
    std::cout << "----------------\n";
}

bool Cli::handleEditorMenuInput(const std::string& line)
{
    auto trim = [](const std::string& s) {
        std::size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return std::string{};
        std::size_t e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    };

    const std::string t = trim(line);

    if (editorWizard_ != EditorWizard::None)
    {
        switch (editorWizard_)
        {
        case EditorWizard::AssignAskPaperId:
            tempPaperId_ = t;
            std::cout << "输入 reviewer 用户名: ";
            editorWizard_ = EditorWizard::AssignAskReviewer;
            return true;
        case EditorWizard::AssignAskReviewer:
        {
            auto payload = buildJsonPayload("ASSIGN " + tempPaperId_ + " " + t);
            if (auto resp = sendRequest(payload))
            {
                printResponse(*resp);
            }
            else
            {
                std::cout << "发送失败\n";
            }
            std::cout << "输入 c 继续指派，m 返回编辑菜单，其他退出向导: ";
            editorWizard_ = EditorWizard::PostAssignPrompt;
            return true;
        }
        case EditorWizard::ViewPaperAskPaperId:
        {
            // 编辑查看论文详情（包含正文）
            auto payload = buildJsonPayload("GET_PAPER " + t);
            if (auto resp = sendRequest(payload))
            {
                printResponse(*resp);
            }
            else
            {
                std::cout << "发送失败\n";
            }
            std::cout << "输入 c 继续查看论文，m 返回编辑菜单，其他退出向导: ";
            editorWizard_ = EditorWizard::PostViewPrompt;
            return true;
        }
        case EditorWizard::ViewReviewsAskPaperId:
        {
            // 编辑查看论文的所有评审
            auto payload = buildJsonPayload("LIST_REVIEWS " + t);
            if (auto resp = sendRequest(payload))
            {
                printResponse(*resp);
            }
            else
            {
                std::cout << "发送失败\n";
            }
            std::cout << "输入 c 继续查看评审，m 返回编辑菜单，其他退出向导: ";
            editorWizard_ = EditorWizard::PostViewPrompt;
            return true;
        }
        case EditorWizard::DecideAskPaperId:
            tempPaperId_ = t;
            std::cout << "输入决定（例如 1: Accept, 2: Reject）: ";
            editorWizard_ = EditorWizard::DecideAskDecision;
            return true;
        case EditorWizard::DecideAskDecision:
        {
            std::string decision;
            if (t == "1") decision = "Accept";
            else if (t == "2") decision = "Reject";
            else decision = t; // 支持直接输入
            tempDecision_ = decision;
            auto payload = buildJsonPayload("DECISION " + tempPaperId_ + " " + tempDecision_);
            if (auto resp = sendRequest(payload))
            {
                printResponse(*resp);
            }
            else
            {
                std::cout << "发送失败\n";
            }
            std::cout << "输入 c 继续决策，m 返回编辑菜单，其他退出向导: ";
            editorWizard_ = EditorWizard::PostDecidePrompt;
            return true;
        }
        case EditorWizard::PostAssignPrompt:
            if (t == "c" || t == "C")
            {
                editorWizard_ = EditorWizard::AssignAskPaperId;
                std::cout << "指派审稿人，输入 paper_id: ";
                return true;
            }
            if (t == "m" || t == "M")
            {
                editorWizard_ = EditorWizard::None;
                printEditorNumericMenu();
                return true;
            }
            editorWizard_ = EditorWizard::None;
            return true;
        case EditorWizard::PostViewPrompt:
            if (t == "c" || t == "C")
            {
                editorWizard_ = EditorWizard::ViewPaperAskPaperId;
                std::cout << "查看审稿状态，输入 paper_id: ";
                return true;
            }
            if (t == "m" || t == "M")
            {
                editorWizard_ = EditorWizard::None;
                printEditorNumericMenu();
                return true;
            }
            editorWizard_ = EditorWizard::None;
            return true;
        case EditorWizard::PostDecidePrompt:
            if (t == "c" || t == "C")
            {
                editorWizard_ = EditorWizard::DecideAskPaperId;
                std::cout << "最终决定，输入 paper_id: ";
                return true;
            }
            if (t == "m" || t == "M")
            {
                editorWizard_ = EditorWizard::None;
                printEditorNumericMenu();
                return true;
            }
            editorWizard_ = EditorWizard::None;
            return true;
        default: break;
        }
    }

    if (t == "1")
    {
        auto payload = buildJsonPayload("LIST_PAPERS");
        if (auto resp = sendRequest(payload))
        {
            printResponse(*resp);
        }
        else
        {
            std::cout << "发送失败\n";
        }
        printEditorNumericMenu();
        return true;
    }
    if (t == "2")
    {
        editorWizard_ = EditorWizard::ViewPaperAskPaperId;
        std::cout << "查看论文详情，输入 paper_id: ";
        return true;
    }
    if (t == "3")
    {
        editorWizard_ = EditorWizard::AssignAskPaperId;
        std::cout << "指派审稿人，输入 paper_id: ";
        return true;
    }
    if (t == "4")
    {
        editorWizard_ = EditorWizard::ViewReviewsAskPaperId;
        std::cout << "查看论文所有评审，输入 paper_id: ";
        return true;
    }
    if (t == "5")
    {
        editorWizard_ = EditorWizard::DecideAskPaperId;
        std::cout << "最终决定，输入 paper_id: ";
        return true;
    }

    return false;
}

} // namespace osp::client

