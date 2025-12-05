#pragma once

#include "common/types.hpp"

#include <string>

namespace osp::domain
{

class User
{
public:
    User() = default;

    User(UserId id, std::string username, Role role)
        : id_(id)
        , username_(std::move(username))
        , role_(role)
    {
    }

    [[nodiscard]] UserId id() const noexcept { return id_; }
    [[nodiscard]] const std::string& username() const noexcept { return username_; }
    [[nodiscard]] Role role() const noexcept { return role_; }

private:
    UserId id_{};
    std::string username_;
    Role role_{Role::Author};
};

} // namespace osp::domain


