#include "paper_service.hpp"

#include "domain/paper.hpp"
#include "domain/permissions.hpp"
#include "domain/review.hpp"
#include "server/handlers/command_utils.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace osp::server
{

osp::protocol::Message PaperService::recommendReviewers(const osp::protocol::Command& cmd,
                                                       const std::optional<osp::domain::Session>& maybeSession)
{
    using osp::protocol::json;

    if (!maybeSession)
    {
        return osp::protocol::makeErrorResponse("AUTH_REQUIRED", "RECOMMEND_REVIEWERS: need to login first");
    }
    if (maybeSession->role != osp::Role::Editor && maybeSession->role != osp::Role::Admin)
    {
        return osp::protocol::makeErrorResponse("PERMISSION_DENIED", "RECOMMEND_REVIEWERS: permission denied");
    }
    if (cmd.args.empty())
    {
        return osp::protocol::makeErrorResponse("MISSING_ARGS", "Usage: RECOMMEND_REVIEWERS <PaperID> [limit]");
    }

    const std::string pidStr = cmd.args[0];
    std::size_t       limit  = 5;
    if (cmd.args.size() >= 2)
    {
        try
        {
            limit = static_cast<std::size_t>(std::stoul(cmd.args[1]));
        }
        catch (...)
        {
            return osp::protocol::makeErrorResponse("INVALID_ARGS", "RECOMMEND_REVIEWERS: invalid limit");
        }
        if (limit == 0) limit = 5;
    }

    // Load paper fields
    std::set<std::string> paperFields;
    {
        std::lock_guard<std::mutex> lock(vfsMutex_);
        const std::string           metaPath = "/papers/" + pidStr + "/meta.txt";
        if (!vfs_.readFile(metaPath))
        {
            return osp::protocol::makeErrorResponse("NOT_FOUND", "Paper not found: " + pidStr);
        }

        const std::string fieldsPath = "/papers/" + pidStr + "/fields.txt";
        if (auto f = vfs_.readFile(fieldsPath))
        {
            paperFields = osp::server::utils::toFieldSet(osp::server::utils::splitFieldsCsv(*f));
        }
    }

    struct ReviewerInfo
    {
        std::string   username;
        std::uint32_t userId{};
    };
    std::vector<ReviewerInfo> reviewers;
    {
        std::lock_guard<std::mutex> lock(authMutex_);
        for (const auto& u : auth_.getAllUsers())
        {
            if (u.role() != osp::Role::Reviewer) continue;
            reviewers.push_back({u.username(), u.id()});
        }
    }

    struct Candidate
    {
        std::string              username;
        std::uint32_t            userId{};
        std::size_t              score{};
        std::vector<std::string> matched;
        std::vector<std::string> reviewerFields;
    };

    std::vector<Candidate> candidates;
    candidates.reserve(reviewers.size());

    for (const auto& r : reviewers)
    {
        std::set<std::string>      reviewerFieldSet;
        std::vector<std::string>   reviewerFields;
        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            vfs_.createDirectory("/system");
            vfs_.createDirectory("/system/reviewer_fields");
            const std::string path = "/system/reviewer_fields/" + std::to_string(r.userId) + ".txt";
            if (auto f = vfs_.readFile(path))
            {
                reviewerFields   = osp::server::utils::splitFieldsCsv(*f);
                reviewerFieldSet = osp::server::utils::toFieldSet(reviewerFields);
            }
        }

        auto              matched = osp::server::utils::intersectionFields(paperFields, reviewerFieldSet);
        const std::size_t score   = matched.size();
        candidates.push_back({r.username, r.userId, score, matched, reviewerFields});
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.username < b.username;
    });

    if (candidates.size() > limit)
    {
        candidates.resize(limit);
    }

    json out;
    out["paperId"] = pidStr;
    {
        json pf = json::array();
        for (const auto& f : paperFields) pf.push_back(f);
        out["paperFields"] = pf;
    }
    json arr = json::array();
    for (const auto& c : candidates)
    {
        json m = json::array();
        for (const auto& f : c.matched) m.push_back(f);
        json rf = json::array();
        for (const auto& f : c.reviewerFields) rf.push_back(f);
        arr.push_back({{"username", c.username},
                       {"userId", c.userId},
                       {"score", c.score},
                       {"matchedFields", m},
                       {"reviewerFields", rf}});
    }
    out["candidates"] = arr;
    return osp::protocol::makeSuccessResponse(out);
}

osp::protocol::Message PaperService::handlePaperCommand(const osp::protocol::Command& cmd,
                                                       const std::optional<osp::domain::Session>& maybeSession)
{
    using osp::protocol::json;
    using osp::domain::Permission;
    using osp::domain::hasPermission;

    if (!maybeSession)
    {
        return osp::protocol::makeErrorResponse("AUTH_REQUIRED", "Authentication required");
    }

    if (cmd.name == "LIST_PAPERS")
    {
        bool isAuthor   = (maybeSession->role == osp::Role::Author);
        bool isReviewer = (maybeSession->role == osp::Role::Reviewer);

        if (isAuthor && !hasPermission(maybeSession->role, Permission::ViewOwnPaperStatus))
        {
            return osp::protocol::makeErrorResponse("PERMISSION_DENIED", "Permission denied");
        }
        if (isReviewer && !hasPermission(maybeSession->role, Permission::DownloadAssignedPapers))
        {
            return osp::protocol::makeErrorResponse("PERMISSION_DENIED", "Permission denied");
        }

        std::optional<std::string> listing;
        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            listing = vfs_.listDirectory("/papers");
        }

        if (!listing)
        {
            return osp::protocol::makeSuccessResponse({{"papers", json::array()}});
        }

        std::stringstream ss(*listing);
        std::string       entry;
        json              papers = json::array();

        while (std::getline(ss, entry))
        {
            if (entry.empty() || entry.back() != '/')
            {
                continue;
            }

            std::string pidStr   = entry.substr(0, entry.size() - 1);
            std::string metaPath = "/papers/" + pidStr + "/meta.txt";

            std::optional<std::string> metaData;
            {
                std::lock_guard<std::mutex> lock(vfsMutex_);
                metaData = vfs_.readFile(metaPath);
            }

            if (!metaData)
            {
                continue;
            }

            std::stringstream metaSS(*metaData);
            std::uint32_t     p_id;
            std::uint32_t     p_authorId;
            std::string       p_status;
            std::string       p_title;

            if (!(metaSS >> p_id >> p_authorId >> p_status))
            {
                continue;
            }
            char dummy;
            metaSS.get(dummy);
            std::getline(metaSS, p_title);

            // Author 只能看自己的
            if (isAuthor && p_authorId != maybeSession->userId)
            {
                continue;
            }

            // Reviewer 只能看分配给自己的
            if (isReviewer)
            {
                std::string reviewersPath = "/papers/" + pidStr + "/reviewers.txt";
                std::optional<std::string> reviewersData;
                {
                    std::lock_guard<std::mutex> lock(vfsMutex_);
                    reviewersData = vfs_.readFile(reviewersPath);
                }

                bool assigned = false;
                if (reviewersData)
                {
                    std::stringstream rss(*reviewersData);
                    std::string       rid;
                    std::string       myIdStr = std::to_string(maybeSession->userId);
                    while (rss >> rid)
                    {
                        if (rid == myIdStr)
                        {
                            assigned = true;
                            break;
                        }
                    }
                }
                if (!assigned)
                {
                    continue;
                }
            }

            papers.push_back({{"id", p_id}, {"title", p_title}, {"status", p_status}, {"authorId", p_authorId}});
        }

        return osp::protocol::makeSuccessResponse({{"papers", papers}});
    }

    if (cmd.name == "SET_PAPER_FIELDS")
    {
        if (cmd.args.empty())
        {
            return osp::protocol::makeErrorResponse("MISSING_ARGS", "Usage: SET_PAPER_FIELDS <PaperID> <fieldsCsv|NONE>");
        }

        const std::string pidStr    = cmd.args[0];
        const std::string paperDir  = "/papers/" + pidStr;
        const std::string metaPath  = paperDir + "/meta.txt";
        const std::string fieldsPath = paperDir + "/fields.txt";

        std::optional<std::string> metaData;
        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            metaData = vfs_.readFile(metaPath);
        }
        if (!metaData)
        {
            return osp::protocol::makeErrorResponse("NOT_FOUND", "Paper not found");
        }

        std::stringstream metaSS(*metaData);
        std::uint32_t     p_id;
        std::uint32_t     p_authorId;
        metaSS >> p_id >> p_authorId;

        const bool isAdmin  = (maybeSession->role == osp::Role::Admin);
        const bool isEditor = (maybeSession->role == osp::Role::Editor);
        const bool isAuthor = (maybeSession->role == osp::Role::Author);

        if (isAuthor && p_authorId != maybeSession->userId)
        {
            return osp::protocol::makeErrorResponse("PERMISSION_DENIED",
                                                    "Permission denied: You can only modify your own papers");
        }
        if (!isAdmin && !isEditor && !isAuthor)
        {
            return osp::protocol::makeErrorResponse("PERMISSION_DENIED", "Permission denied");
        }

        std::string fieldsCsv;
        if (cmd.args.size() >= 2)
        {
            fieldsCsv = cmd.args[1];
        }

        std::string toWrite;
        if (fieldsCsv.empty() || fieldsCsv == "NONE" || fieldsCsv == "none" || fieldsCsv == "-")
        {
            toWrite = "";
        }
        else
        {
            auto fields = osp::server::utils::splitFieldsCsv(fieldsCsv);
            for (std::size_t i = 0; i < fields.size(); ++i)
            {
                if (i) toWrite += ',';
                toWrite += fields[i];
            }
        }

        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            if (!vfs_.writeFile(fieldsPath, toWrite))
            {
                return osp::protocol::makeErrorResponse("FS_ERROR", "Failed to save paper fields");
            }
        }

        json fieldsArr = json::array();
        for (const auto& f : osp::server::utils::splitFieldsCsv(toWrite))
        {
            fieldsArr.push_back(f);
        }

        return osp::protocol::makeSuccessResponse(
            {{"message", "Paper fields updated"}, {"paperId", pidStr}, {"fields", fieldsArr}});
    }

    if (cmd.name == "GET_PAPER")
    {
        if (cmd.args.empty())
        {
            return osp::protocol::makeErrorResponse("MISSING_ARGS", "Usage: GET_PAPER <PaperID>");
        }

        std::string pidStr     = cmd.args[0];
        std::string metaPath   = "/papers/" + pidStr + "/meta.txt";
        std::string fieldsPath = "/papers/" + pidStr + "/fields.txt";

        std::optional<std::string> metaData;
        std::optional<std::string> fieldsData;
        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            metaData = vfs_.readFile(metaPath);
            fieldsData = vfs_.readFile(fieldsPath);
        }

        if (!metaData)
        {
            return osp::protocol::makeErrorResponse("NOT_FOUND", "Paper not found");
        }

        std::stringstream metaSS(*metaData);
        std::uint32_t     p_id;
        std::uint32_t     p_authorId;
        std::string       p_status;
        std::string       p_title;

        metaSS >> p_id >> p_authorId >> p_status;
        char dummy;
        metaSS.get(dummy);
        std::getline(metaSS, p_title);

        if (maybeSession->role == osp::Role::Author && p_authorId != maybeSession->userId)
        {
            return osp::protocol::makeErrorResponse("PERMISSION_DENIED",
                                                    "Permission denied: You can only view your own papers");
        }

        if (maybeSession->role == osp::Role::Reviewer)
        {
            std::string reviewersPath = "/papers/" + pidStr + "/reviewers.txt";
            std::optional<std::string> reviewersData;
            {
                std::lock_guard<std::mutex> lock(vfsMutex_);
                reviewersData = vfs_.readFile(reviewersPath);
            }

            bool assigned = false;
            if (reviewersData)
            {
                std::stringstream rss(*reviewersData);
                std::string       rid;
                std::string       myIdStr = std::to_string(maybeSession->userId);
                while (rss >> rid)
                {
                    if (rid == myIdStr)
                    {
                        assigned = true;
                        break;
                    }
                }
            }
            if (!assigned)
            {
                return osp::protocol::makeErrorResponse("PERMISSION_DENIED",
                                                        "Permission denied: You are not assigned to this paper");
            }
        }

        std::string contentPath = "/papers/" + pidStr + "/content.txt";
        std::optional<std::string> contentData;
        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            contentData = vfs_.readFile(contentPath);
        }

        json data;
        data["id"]       = p_id;
        data["title"]    = p_title;
        data["status"]   = p_status;
        data["authorId"] = p_authorId;
        data["content"]  = contentData ? *contentData : "";

        json fieldsArr = json::array();
        if (fieldsData)
        {
            for (const auto& f : osp::server::utils::splitFieldsCsv(*fieldsData))
            {
                fieldsArr.push_back(f);
            }
        }
        data["fields"] = fieldsArr;

        return osp::protocol::makeSuccessResponse(data);
    }

    if (cmd.name == "SUBMIT")
    {
        if (!hasPermission(maybeSession->role, Permission::UploadPaper))
        {
            return osp::protocol::makeErrorResponse("PERMISSION_DENIED", "Permission denied: Author role required");
        }

        if (cmd.args.size() < 2)
        {
            return osp::protocol::makeErrorResponse("MISSING_ARGS", "Usage: SUBMIT <Title> <Content>");
        }

        std::string title = cmd.args[0];

        // 从 rawArgs 中提取 content
        std::string content;
        size_t      titlePos = cmd.rawArgs.find(title);
        if (titlePos != std::string::npos)
        {
            size_t contentStart = titlePos + title.length();
            while (contentStart < cmd.rawArgs.length()
                   && std::isspace(static_cast<unsigned char>(cmd.rawArgs[contentStart])))
            {
                contentStart++;
            }
            content = cmd.rawArgs.substr(contentStart);
        }

        if (content.empty())
        {
            return osp::protocol::makeErrorResponse("INVALID_ARGS", "SUBMIT: Content is empty");
        }

        std::uint32_t pid      = nextPaperId();
        std::string   paperDir = "/papers/" + std::to_string(pid);

        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            vfs_.createDirectory("/papers");

            if (!vfs_.createDirectory(paperDir))
            {
                return osp::protocol::makeErrorResponse("FS_ERROR", "Failed to create paper directory");
            }

            if (!vfs_.writeFile(paperDir + "/content.txt", content))
            {
                return osp::protocol::makeErrorResponse("FS_ERROR", "Failed to save paper content");
            }

            std::ostringstream meta;
            meta << pid << "\n" << maybeSession->userId << "\n"
                 << osp::domain::paperStatusToString(osp::domain::PaperStatus::Submitted) << "\n" << title;

            if (!vfs_.writeFile(paperDir + "/meta.txt", meta.str()))
            {
                return osp::protocol::makeErrorResponse("FS_ERROR", "Failed to save paper metadata");
            }
        }

        return osp::protocol::makeSuccessResponse({{"message", "Paper submitted successfully"}, {"paperId", pid}});
    }

    if (cmd.name == "REVISE")
    {
        // 只有Author角色可以执行REVISE命令
        if (maybeSession->role != osp::Role::Author)
        {
            return osp::protocol::makeErrorResponse("PERMISSION_DENIED",
                                                    "Permission denied: Only Author role can revise papers");
        }

        if (cmd.rawArgs.empty())
        {
            return osp::protocol::makeErrorResponse("MISSING_ARGS", "Usage: REVISE <PaperID> <NewContent...>");
        }

        std::istringstream iss(cmd.rawArgs);
        std::string        pidStr;
        iss >> pidStr;
        if (pidStr.empty())
        {
            return osp::protocol::makeErrorResponse("MISSING_ARGS", "REVISE: missing paper_id");
        }

        std::string newContent;
        std::getline(iss, newContent);
        if (!newContent.empty() && newContent.front() == ' ')
        {
            newContent.erase(newContent.begin());
        }
        if (newContent.empty())
        {
            return osp::protocol::makeErrorResponse("INVALID_ARGS", "REVISE: content is empty");
        }

        const std::string paperDir     = "/papers/" + pidStr;
        const std::string metaPath     = paperDir + "/meta.txt";
        const std::string contentPath  = paperDir + "/content.txt";
        const std::string revisionsDir = paperDir + "/revisions";

        // 读 meta，校验作者
        std::optional<std::string> metaData;
        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            metaData = vfs_.readFile(metaPath);
        }
        if (!metaData)
        {
            return osp::protocol::makeErrorResponse("NOT_FOUND", "Paper not found");
        }

        std::stringstream metaSS(*metaData);
        std::uint32_t     p_id;
        std::uint32_t     p_authorId;
        std::string       p_status;
        std::string       p_title;

        if (!(metaSS >> p_id >> p_authorId >> p_status))
        {
            return osp::protocol::makeErrorResponse("FS_ERROR", "REVISE failed: bad meta format");
        }
        char dummy;
        metaSS.get(dummy);
        std::getline(metaSS, p_title);

        if (p_authorId != maybeSession->userId)
        {
            return osp::protocol::makeErrorResponse("PERMISSION_DENIED",
                                                    "Permission denied: You can only revise papers in your own paper list");
        }

        std::uint32_t newVersion = 1;
        {
            std::lock_guard<std::mutex> lock(vfsMutex_);

            if (!vfs_.listDirectory(revisionsDir))
            {
                vfs_.createDirectory(revisionsDir);
            }

            if (auto listing = vfs_.listDirectory(revisionsDir))
            {
                std::stringstream ss(*listing);
                std::string       entry;
                std::uint32_t     maxV = 0;
                while (std::getline(ss, entry))
                {
                    if (entry.empty() || entry.back() == '/')
                    {
                        continue;
                    }
                    if (entry.size() >= 6 && entry.front() == 'v' && entry.rfind(".txt") == entry.size() - 4)
                    {
                        const std::string numStr = entry.substr(1, entry.size() - 1 - 4);
                        try
                        {
                            std::uint32_t v = static_cast<std::uint32_t>(std::stoul(numStr));
                            if (v > maxV) maxV = v;
                        }
                        catch (...)
                        {
                        }
                    }
                }
                newVersion = maxV + 1;
            }

            const std::string revPath     = revisionsDir + "/v" + std::to_string(newVersion) + ".txt";
            const auto        oldContent  = vfs_.readFile(contentPath);
            if (!vfs_.writeFile(revPath, oldContent ? *oldContent : std::string{}))
            {
                return osp::protocol::makeErrorResponse("FS_ERROR", "REVISE failed: cannot save revision history");
            }

            if (!vfs_.writeFile(contentPath, newContent))
            {
                return osp::protocol::makeErrorResponse("FS_ERROR", "REVISE failed: cannot write new content");
            }

            std::ostringstream newMeta;
            newMeta << p_id << "\n" << p_authorId << "\n"
                    << osp::domain::paperStatusToString(osp::domain::PaperStatus::Submitted) << "\n" << p_title;
            if (!vfs_.writeFile(metaPath, newMeta.str()))
            {
                return osp::protocol::makeErrorResponse("FS_ERROR", "REVISE failed: cannot update meta");
            }
        }

        return osp::protocol::makeSuccessResponse(
            {{"message", "Revision submitted successfully"}, {"paperId", pidStr}, {"revision", newVersion}});
    }

    if (cmd.name == "ASSIGN")
    {
        if (!hasPermission(maybeSession->role, Permission::AssignReviewers))
        {
            return osp::protocol::makeErrorResponse("PERMISSION_DENIED", "Permission denied: Editor role required");
        }

        if (cmd.args.size() < 2)
        {
            return osp::protocol::makeErrorResponse("MISSING_ARGS", "Usage: ASSIGN <PaperID> <ReviewerUsername>");
        }

        std::string pidStr       = cmd.args[0];
        std::string reviewerName = cmd.args[1];

        std::string paperDir = "/papers/" + pidStr;
        std::string metaPath = paperDir + "/meta.txt";

        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            if (!vfs_.readFile(metaPath))
            {
                return osp::protocol::makeErrorResponse("NOT_FOUND", "Paper not found: " + pidStr);
            }
        }

        std::optional<osp::UserId> reviewerIdOpt;
        {
            std::lock_guard<std::mutex> lock(authMutex_);
            reviewerIdOpt = auth_.getUserId(reviewerName);
        }

        if (!reviewerIdOpt)
        {
            return osp::protocol::makeErrorResponse("NOT_FOUND", "User not found: " + reviewerName);
        }

        std::string reviewersPath    = paperDir + "/reviewers.txt";
        std::string currentReviewers;

        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            auto existing = vfs_.readFile(reviewersPath);
            if (existing)
            {
                currentReviewers = *existing;
            }
        }

        std::string newEntry = std::to_string(*reviewerIdOpt);

        std::stringstream rss(currentReviewers);
        std::string       rid;
        bool              alreadyAssigned = false;
        while (rss >> rid)
        {
            if (rid == newEntry)
            {
                alreadyAssigned = true;
                break;
            }
        }

        if (alreadyAssigned)
        {
            return osp::protocol::makeErrorResponse("ALREADY_ASSIGNED",
                                                    "Reviewer " + reviewerName + " is already assigned to this paper");
        }

        currentReviewers += newEntry + "\n";

        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            if (!vfs_.writeFile(reviewersPath, currentReviewers))
            {
                return osp::protocol::makeErrorResponse("FS_ERROR", "Failed to save assignment");
            }
        }

        return osp::protocol::makeSuccessResponse(
            {{"message", "Reviewer assigned"},
             {"paperId", pidStr},
             {"reviewer", reviewerName},
             {"reviewerId", *reviewerIdOpt}});
    }

    if (cmd.name == "REVIEW")
    {
        if (!hasPermission(maybeSession->role, Permission::UploadReview))
        {
            return osp::protocol::makeErrorResponse("PERMISSION_DENIED", "Permission denied: Reviewer role required");
        }

        if (cmd.args.size() < 3)
        {
            return osp::protocol::makeErrorResponse(
                "MISSING_ARGS",
                "Usage: REVIEW <PaperID> <Decision> <Comments...>\nDecisions: ACCEPT, REJECT, MINOR, MAJOR");
        }

        std::string pidStr      = cmd.args[0];
        std::string decisionStr = cmd.args[1];

        std::string comments;
        size_t      decisionPos = cmd.rawArgs.find(decisionStr);
        if (decisionPos != std::string::npos)
        {
            size_t commentsStart = decisionPos + decisionStr.length();
            while (commentsStart < cmd.rawArgs.length()
                   && std::isspace(static_cast<unsigned char>(cmd.rawArgs[commentsStart])))
            {
                commentsStart++;
            }
            comments = cmd.rawArgs.substr(commentsStart);
        }

        if (comments.empty())
        {
            return osp::protocol::makeErrorResponse("INVALID_ARGS", "REVIEW: Comments are required");
        }

        auto decision = osp::domain::stringToReviewDecision(decisionStr);
        if (!decision)
        {
            return osp::protocol::makeErrorResponse("INVALID_ARGS", "Invalid decision. Allowed: ACCEPT, REJECT, MINOR, MAJOR");
        }

        std::string paperDir      = "/papers/" + pidStr;
        std::string reviewersPath = paperDir + "/reviewers.txt";

        std::optional<std::string> reviewersData;
        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            reviewersData = vfs_.readFile(reviewersPath);
        }

        bool assigned = false;
        if (reviewersData)
        {
            std::stringstream rss(*reviewersData);
            std::string       rid;
            std::string       myIdStr = std::to_string(maybeSession->userId);
            while (rss >> rid)
            {
                if (rid == myIdStr)
                {
                    assigned = true;
                    break;
                }
            }
        }

        if (!assigned)
        {
            return osp::protocol::makeErrorResponse("PERMISSION_DENIED",
                                                    "Permission denied: You are not assigned to review this paper");
        }

        std::string reviewsDir  = paperDir + "/reviews";
        std::string reviewPath  = reviewsDir + "/" + std::to_string(maybeSession->userId) + ".txt";

        std::ostringstream reviewContent;
        reviewContent << decisionStr << "\n" << comments;

        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            vfs_.createDirectory(reviewsDir);

            if (!vfs_.writeFile(reviewPath, reviewContent.str()))
            {
                return osp::protocol::makeErrorResponse("FS_ERROR", "Failed to save review");
            }
        }

        return osp::protocol::makeSuccessResponse(
            {{"message", "Review submitted successfully"}, {"paperId", pidStr}, {"decision", decisionStr}});
    }

    if (cmd.name == "LIST_REVIEWS")
    {
        if (cmd.args.empty())
        {
            return osp::protocol::makeErrorResponse("MISSING_ARGS", "Usage: LIST_REVIEWS <PaperID>");
        }

        std::string pidStr = cmd.args[0];

        std::string metaPath = "/papers/" + pidStr + "/meta.txt";
        std::optional<std::string> metaData;
        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            metaData = vfs_.readFile(metaPath);
        }

        if (!metaData)
        {
            return osp::protocol::makeErrorResponse("NOT_FOUND", "Paper not found");
        }

        std::stringstream metaSS(*metaData);
        std::uint32_t     p_id;
        std::uint32_t     p_authorId;
        metaSS >> p_id >> p_authorId;

        bool isEditor = (maybeSession->role == osp::Role::Editor);
        bool isAdmin  = (maybeSession->role == osp::Role::Admin);
        bool isAuthor = (maybeSession->role == osp::Role::Author);

        if (isAuthor && p_authorId != maybeSession->userId)
        {
            return osp::protocol::makeErrorResponse("PERMISSION_DENIED",
                                                    "Permission denied: You can only view reviews for your own papers");
        }

        if (!isEditor && !isAdmin && !isAuthor)
        {
            return osp::protocol::makeErrorResponse("PERMISSION_DENIED", "Permission denied");
        }

        std::string reviewsDir = "/papers/" + pidStr + "/reviews";
        std::optional<std::string> listing;
        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            listing = vfs_.listDirectory(reviewsDir);
        }

        if (!listing)
        {
            return osp::protocol::makeSuccessResponse({{"reviews", json::array()}});
        }

        std::stringstream ss(*listing);
        std::string       entry;
        json              reviews = json::array();

        while (std::getline(ss, entry))
        {
            if (entry.empty())
                continue;

            std::string reviewPath = reviewsDir + "/" + entry;
            std::optional<std::string> reviewContent;
            {
                std::lock_guard<std::mutex> lock(vfsMutex_);
                reviewContent = vfs_.readFile(reviewPath);
            }

            if (!reviewContent)
                continue;

            std::stringstream rss(*reviewContent);
            std::string       decision;
            std::string       comments;
            std::getline(rss, decision);
            std::string line;
            while (std::getline(rss, line))
            {
                comments += line + "\n";
            }
            if (!comments.empty() && comments.back() == '\n')
            {
                comments.pop_back();
            }

            std::string reviewerIdStr = entry.substr(0, entry.find('.'));

            reviews.push_back({{"reviewerId", reviewerIdStr}, {"decision", decision}, {"comments", comments}});
        }

        return osp::protocol::makeSuccessResponse({{"reviews", reviews}});
    }

    if (cmd.name == "DECISION")
    {
        if (!hasPermission(maybeSession->role, Permission::MakeFinalDecision))
        {
            return osp::protocol::makeErrorResponse("PERMISSION_DENIED", "Permission denied: Editor role required");
        }

        if (cmd.args.size() < 2)
        {
            return osp::protocol::makeErrorResponse("MISSING_ARGS", "Usage: DECISION <PaperID> <Decision> (ACCEPT/REJECT)");
        }

        std::string pidStr      = cmd.args[0];
        std::string decisionStr = cmd.args[1];

        if (decisionStr != "ACCEPT" && decisionStr != "REJECT")
        {
            return osp::protocol::makeErrorResponse("INVALID_ARGS", "Invalid decision. Use ACCEPT or REJECT");
        }

        std::string metaPath = "/papers/" + pidStr + "/meta.txt";
        std::optional<std::string> metaData;
        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            metaData = vfs_.readFile(metaPath);
        }

        if (!metaData)
        {
            return osp::protocol::makeErrorResponse("NOT_FOUND", "Paper not found");
        }

        std::stringstream metaSS(*metaData);
        std::uint32_t     p_id;
        std::uint32_t     p_authorId;
        std::string       p_status;
        std::string       p_title;

        metaSS >> p_id >> p_authorId >> p_status;
        char dummy;
        metaSS.get(dummy);
        std::getline(metaSS, p_title);

        std::string newStatus = (decisionStr == "ACCEPT") ? "Accepted" : "Rejected";

        std::ostringstream newMeta;
        newMeta << p_id << "\n" << p_authorId << "\n" << newStatus << "\n" << p_title;

        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            if (!vfs_.writeFile(metaPath, newMeta.str()))
            {
                return osp::protocol::makeErrorResponse("FS_ERROR", "Failed to update paper status");
            }
        }

        return osp::protocol::makeSuccessResponse({{"message", "Paper decision updated"}, {"paperId", pidStr}, {"status", newStatus}});
    }

    return osp::protocol::makeErrorResponse("UNKNOWN_COMMAND", "Unknown paper command: " + cmd.name);
}

std::uint32_t PaperService::nextPaperId()
{
    std::lock_guard<std::mutex> lock(vfsMutex_);

    std::string   path   = "/system/next_paper_id";
    std::uint32_t nextId = 1;

    vfs_.createDirectory("/system");

    auto data = vfs_.readFile(path);
    if (data)
    {
        try
        {
            nextId = std::stoul(*data);
        }
        catch (...)
        {
            nextId = 1;
        }
    }

    vfs_.writeFile(path, std::to_string(nextId + 1));

    return nextId;
}

} // namespace osp::server



