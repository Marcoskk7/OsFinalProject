#pragma once

#include <cstdint>
#include <string>

namespace osp
{

using UserId = std::uint32_t;
using PaperId = std::uint32_t;
using ReviewId = std::uint32_t;

enum class Role
{
    Author,
    Reviewer,
    Editor,
    Admin
};

struct Credentials
{
    std::string username;
    std::string password;
};

} // namespace osp


