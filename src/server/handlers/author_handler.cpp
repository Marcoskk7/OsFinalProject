#include "author_handler.hpp"

namespace osp::server
{

std::optional<osp::protocol::Message> AuthorHandler::tryHandle(const osp::protocol::Command& cmd,
                                                              const std::optional<osp::domain::Session>& maybeSession)
{
    if (cmd.name != "LIST_PAPERS" && cmd.name != "SUBMIT" && cmd.name != "GET_PAPER" && cmd.name != "REVISE"
        && cmd.name != "SET_PAPER_FIELDS")
    {
        return std::nullopt;
    }

    return paperService_.handlePaperCommand(cmd, maybeSession);
}

} // namespace osp::server




