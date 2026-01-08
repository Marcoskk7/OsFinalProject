#include "reviewer_handler.hpp"

namespace osp::server
{

std::optional<osp::protocol::Message> ReviewerHandler::tryHandle(const osp::protocol::Command& cmd,
                                                                const std::optional<osp::domain::Session>& maybeSession)
{
    if (cmd.name != "REVIEW")
    {
        return std::nullopt;
    }

    return paperService_.handlePaperCommand(cmd, maybeSession);
}

} // namespace osp::server




