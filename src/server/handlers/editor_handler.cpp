#include "editor_handler.hpp"

namespace osp::server
{

std::optional<osp::protocol::Message> EditorHandler::tryHandle(const osp::protocol::Command& cmd,
                                                              const std::optional<osp::domain::Session>& maybeSession)
{
    if (cmd.name != "RECOMMEND_REVIEWERS" && cmd.name != "ASSIGN_REVIEWER" && cmd.name != "VIEW_REVIEW_STATUS"
        && cmd.name != "MAKE_FINAL_DECISION" && cmd.name != "ASSIGN" && cmd.name != "LIST_REVIEWS"
        && cmd.name != "DECISION")
    {
        return std::nullopt;
    }

    if (cmd.name == "RECOMMEND_REVIEWERS")
    {
        return paperService_.recommendReviewers(cmd, maybeSession);
    }

    // Editor 便捷封装命令：转成 PaperService 支持的标准命令
    if (cmd.name == "ASSIGN_REVIEWER")
    {
        if (cmd.args.size() < 2)
        {
            return osp::protocol::makeErrorResponse("MISSING_ARGS",
                                                    "ASSIGN_REVIEWER: missing paper_id or reviewer_username");
        }
        osp::protocol::Command assignCmd = cmd;
        assignCmd.name = "ASSIGN";
        return paperService_.handlePaperCommand(assignCmd, maybeSession);
    }
    if (cmd.name == "VIEW_REVIEW_STATUS")
    {
        if (cmd.args.empty())
        {
            return osp::protocol::makeErrorResponse("MISSING_ARGS", "VIEW_REVIEW_STATUS: missing paper_id");
        }
        osp::protocol::Command listCmd = cmd;
        listCmd.name = "LIST_REVIEWS";
        return paperService_.handlePaperCommand(listCmd, maybeSession);
    }
    if (cmd.name == "MAKE_FINAL_DECISION")
    {
        if (cmd.args.size() < 2)
        {
            return osp::protocol::makeErrorResponse("MISSING_ARGS", "MAKE_FINAL_DECISION: missing paper_id or decision");
        }
        osp::protocol::Command decisionCmd = cmd;
        decisionCmd.name = "DECISION";
        return paperService_.handlePaperCommand(decisionCmd, maybeSession);
    }

    // 标准编辑流程命令：直接交给 PaperService
    return paperService_.handlePaperCommand(cmd, maybeSession);
}

} // namespace osp::server




