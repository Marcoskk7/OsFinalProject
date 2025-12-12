#include "permissions.hpp"

namespace osp::domain
{

bool hasPermission(Role role, Permission permission) noexcept
{
    using P = Permission;

    switch (role)
    {
    case Role::Author:
        switch (permission)
        {
        case P::UploadPaper:
        case P::SubmitRevision:
        case P::ViewOwnPaperStatus:
        case P::DownloadOwnReviews:
            return true;
        default:
            return false;
        }

    case Role::Reviewer:
        switch (permission)
        {
        case P::DownloadAssignedPapers:
        case P::UploadReview:
        case P::ViewAssignedPaperStatus:
            return true;
        default:
            return false;
        }

    case Role::Editor:
        switch (permission)
        {
        case P::AssignReviewers:
        case P::MakeFinalDecision:
        case P::ViewSystemStatus:
            // Editor 通常也需要查看系统/论文状态
            return true;
        default:
            return false;
        }

    case Role::Admin:
        // Admin 被视为拥有最高权限，这里简单地允许所有操作
        return true;
    }

    return false;
}

} // namespace osp::domain



