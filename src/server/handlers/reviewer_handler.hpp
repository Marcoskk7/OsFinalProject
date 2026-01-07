#pragma once

#include "common/protocol.hpp"
#include "domain/auth.hpp"
#include "server/services/paper_service.hpp"

#include <optional>

namespace osp::server
{

class ReviewerHandler
{
public:
    explicit ReviewerHandler(PaperService& paperService)
        : paperService_(paperService)
    {
    }

    std::optional<osp::protocol::Message> tryHandle(const osp::protocol::Command& cmd,
                                                    const std::optional<osp::domain::Session>& maybeSession);

private:
    PaperService& paperService_;
};

} // namespace osp::server



