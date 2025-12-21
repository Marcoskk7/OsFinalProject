#pragma once

#include "common/types.hpp"

#include <string>

namespace osp::domain
{

enum class PaperStatus
{
    Submitted,
    UnderReview,
    Accepted,
    Rejected
};

struct Paper
{
    PaperId id{};
    UserId  author{};
    std::string title;
    PaperStatus status{PaperStatus::Submitted};
};

// 辅助函数：将状态转换为字符串
inline std::string paperStatusToString(PaperStatus s)
{
    switch (s)
    {
    case PaperStatus::Submitted:   return "Submitted";
    case PaperStatus::UnderReview: return "UnderReview";
    case PaperStatus::Accepted:    return "Accepted";
    case PaperStatus::Rejected:    return "Rejected";
    default:                       return "Unknown";
    }
}

} // namespace osp::domain


