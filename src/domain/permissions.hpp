#pragma once

#include "roles.hpp"

namespace osp::domain
{

// 表示系统中可能出现的高层动作，用于做权限检查。
// 这些枚举值对应课程指导书里的典型操作，但目前仅用于判断角色是否有权限，
// 具体业务流程和命令尚未实现。
enum class Permission
{
    // Author 相关
    UploadPaper,           // 上传论文
    SubmitRevision,        // 提交修订版
    ViewOwnPaperStatus,    // 查看自己论文的评审状态
    DownloadOwnReviews,    // 下载针对自己论文的评审意见

    // Reviewer 相关
    DownloadAssignedPapers, // 下载被分配给自己的论文
    UploadReview,           // 上传评审报告
    ViewAssignedPaperStatus,// 查看被分配论文的状态

    // Editor 相关
    AssignReviewers,       // 分配审稿人
    MakeFinalDecision,     // 给出最终接收/拒稿决定

    // Admin 相关
    ManageUsers,           // 用户管理（增删改、重置密码等）
    ManageBackups,         // 备份创建/恢复
    ViewSystemStatus       // 查看系统运行状态（缓存命中率、磁盘使用等）
};

// 核心权限检查函数：给定角色和目标 Permission，返回是否允许执行。
bool hasPermission(Role role, Permission permission) noexcept;

} // namespace osp::domain



