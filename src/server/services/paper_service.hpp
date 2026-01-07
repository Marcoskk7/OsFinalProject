#pragma once

#include "common/protocol.hpp"
#include "domain/auth.hpp"
#include "server/filesystem/vfs.hpp"

#include <cstdint>
#include <mutex>
#include <optional>

namespace osp::server
{

class PaperService
{
public:
    PaperService(osp::fs::Vfs& vfs, std::mutex& vfsMutex, osp::domain::AuthService& auth, std::mutex& authMutex)
        : vfs_(vfs)
        , vfsMutex_(vfsMutex)
        , auth_(auth)
        , authMutex_(authMutex)
    {
    }

    // 处理论文相关命令：LIST_PAPERS / SUBMIT / GET_PAPER / ASSIGN / REVIEW / LIST_REVIEWS / DECISION / REVISE / SET_PAPER_FIELDS
    osp::protocol::Message handlePaperCommand(const osp::protocol::Command& cmd,
                                              const std::optional<osp::domain::Session>& maybeSession);

    // Editor/Admin：审稿人推荐
    osp::protocol::Message recommendReviewers(const osp::protocol::Command& cmd,
                                              const std::optional<osp::domain::Session>& maybeSession);

private:
    std::uint32_t nextPaperId();

    osp::fs::Vfs&             vfs_;
    std::mutex&               vfsMutex_;
    osp::domain::AuthService& auth_;
    std::mutex&               authMutex_;
};

} // namespace osp::server


