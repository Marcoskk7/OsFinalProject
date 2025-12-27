#pragma once

#include "common/protocol.hpp"

#include <string>
#include <set>
#include <optional>

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
    // - 对后续业务命令自动携带 sessionId。
    void run();

private:
    // 根据当前 sessionId 状态，将用户输入的一行命令封装为 JSON payload。
    [[nodiscard]] osp::protocol::json buildJsonPayload(const std::string& line) const;

    // 发送请求并接收响应，返回响应消息
    std::optional<osp::protocol::Message> sendRequest(const osp::protocol::json& payload);

    // 当客户端发送 LOGIN 命令并收到成功响应时，从响应 payload 中解析并保存新的 sessionId。
    void handleLoginResponse(const osp::protocol::Message& resp);

    // 格式化输出 JSON 响应（pretty print 或用户友好的格式）
    void printResponse(const osp::protocol::Message& resp) const;

    // 打印通用指引（PING/LOGIN/退出等）。
    void printGeneralGuide() const;

    // 登录成功后，基于当前角色打印可用命令提示。
    void printRoleGuide() const;

    // 处理作者数字菜单的输入与多步流程。
    bool handleAuthorMenuInput(const std::string& line);
    void printAuthorNumericMenu() const;

    // 处理审稿人数字菜单的输入与多步流程。
    bool handleReviewerMenuInput(const std::string& line);
    void printReviewerNumericMenu() const;

    // 处理管理员数字菜单的输入与多步流程。
    bool handleAdminMenuInput(const std::string& line);
    void printAdminNumericMenu() const;

    // 处理编辑数字菜单的输入与多步流程。
    bool handleEditorMenuInput(const std::string& line);
    void printEditorNumericMenu() const;

private:
    std::string    host_;
    unsigned short port_{};
    std::string    sessionId_;          // 当前会话 ID，空字符串表示未登录
    std::string    currentUser_;        // 当前登录用户名
    std::string    currentRole_;        // 当前登录角色（Admin / Editor / ...）
    std::string    currentPath_ = "/";  // 客户端维护的"当前目录"（仅影响默认 LIST 等命令）

    // 作者数字菜单的临时状态
    enum class AuthorWizard
    {
        None,
        SubmitAskTitle,
        SubmitAskFields,
        SubmitAskContent,
        ViewAskPaperId,
        ViewReviewsAskPaperId,
        ReviseAskPaperId,
        ReviseAskContent,
        PostSubmitPrompt,
        PostViewPrompt,
        PostViewReviewsPrompt,
        PostRevisePrompt
    };
    AuthorWizard authorWizard_ = AuthorWizard::None;
    std::string  tempTitle_;
    std::string  tempFieldsCsv_;

    // 审稿人数字菜单的临时状态
    enum class ReviewerWizard
    {
        None,
        ViewAskPaperId,
        ReviewAskPaperId,
        ReviewAskDecision,
        ReviewAskComments,
        PostViewPrompt,
        PostReviewPrompt
    };
    ReviewerWizard reviewerWizard_ = ReviewerWizard::None;

    // 管理员数字菜单的临时状态
    enum class AdminWizard
    {
        None,
        AddReviewerAskName,
        AddReviewerAskPassword,
        AddReviewerAskFields,
        RemoveUserAskName,
        UpdateRoleAskName,
        UpdateRoleAskRole,
        UpdateRoleAskFields,
        ResetPwdAskName,
        ResetPwdAskNewPwd,
        BackupAskPath,
        RestoreAskPath,
        PostAddPrompt,
        PostRemovePrompt,
        PostUpdatePrompt,
        PostResetPwdPrompt,
        PostBackupPrompt,
        PostRestorePrompt
    };
    AdminWizard  adminWizard_ = AdminWizard::None;
    std::string  tempUsername_;
    std::string  tempPassword_;
    std::string  tempRole_;
    std::string  tempPath_;
    std::set<std::string> tempFields_;

    // 编辑数字菜单的临时状态
    enum class EditorWizard
    {
        None,
        AssignAskPaperId,
        AssignPickRecommended,
        AssignAskReviewer,
        ViewAskPaperId,
        ViewPaperAskPaperId,
        ViewReviewsAskPaperId,
        DecideAskPaperId,
        DecideAskDecision,
        PostAssignPrompt,
        PostViewPrompt,
        PostDecidePrompt
    };
    EditorWizard editorWizard_ = EditorWizard::None;
    std::string  tempPaperId_;
    std::string  tempDecision_;
};

} // namespace osp::client

