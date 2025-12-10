#pragma once

#include "common/types.hpp"

#include <string>
#include <optional>

namespace osp::domain
{

enum class ReviewDecision
{
    Accept,
    MinorRevision,
    MajorRevision,
    Reject
};

inline std::string reviewDecisionToString(ReviewDecision d)
{
    switch (d)
    {
    case ReviewDecision::Accept: return "ACCEPT";
    case ReviewDecision::MinorRevision: return "MINOR";
    case ReviewDecision::MajorRevision: return "MAJOR";
    case ReviewDecision::Reject: return "REJECT";
    default: return "UNKNOWN";
    }
}

inline std::optional<ReviewDecision> stringToReviewDecision(const std::string& s)
{
    if (s == "ACCEPT") return ReviewDecision::Accept;
    if (s == "MINOR") return ReviewDecision::MinorRevision;
    if (s == "MAJOR") return ReviewDecision::MajorRevision;
    if (s == "REJECT") return ReviewDecision::Reject;
    return std::nullopt;
}

struct Review
{
    ReviewId id{};
    PaperId paper{};
    UserId reviewer{};
    ReviewDecision decision{ReviewDecision::Reject};
    std::string comments;
};

} // namespace osp::domain


