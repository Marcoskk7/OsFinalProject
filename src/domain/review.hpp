#pragma once

#include "common/types.hpp"

#include <string>

namespace osp::domain
{

enum class ReviewDecision
{
    Accept,
    MinorRevision,
    MajorRevision,
    Reject
};

struct Review
{
    ReviewId id{};
    PaperId paper{};
    UserId reviewer{};
    ReviewDecision decision{ReviewDecision::Reject};
    std::string comments;
};

} // namespace osp::domain


