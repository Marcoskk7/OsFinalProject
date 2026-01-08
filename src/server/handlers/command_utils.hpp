#pragma once

#include "domain/roles.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <vector>

namespace osp::server::utils
{

// Role <-> string
inline std::string roleToString(osp::Role role)
{
    switch (role)
    {
    case osp::Role::Author: return "Author";
    case osp::Role::Reviewer: return "Reviewer";
    case osp::Role::Editor: return "Editor";
    case osp::Role::Admin: return "Admin";
    }
    return "Unknown";
}

inline osp::Role stringToRole(const std::string& s)
{
    if (s == "Reviewer") return osp::Role::Reviewer;
    if (s == "Editor") return osp::Role::Editor;
    if (s == "Admin") return osp::Role::Admin;
    return osp::Role::Author;
}

inline std::string trimCopy(const std::string& s)
{
    std::size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    std::size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

inline std::string normalizeFieldToken(std::string s)
{
    s = trimCopy(s);
    for (char& c : s)
    {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

inline std::vector<std::string> splitFieldsCsv(const std::string& csv)
{
    std::vector<std::string> out;
    std::string              current;
    for (char ch : csv)
    {
        if (ch == ',')
        {
            std::string tok = normalizeFieldToken(current);
            if (!tok.empty()) out.push_back(tok);
            current.clear();
            continue;
        }
        current.push_back(ch);
    }
    {
        std::string tok = normalizeFieldToken(current);
        if (!tok.empty()) out.push_back(tok);
    }

    // de-dup while keeping stable order
    std::set<std::string> seen;
    std::vector<std::string> unique;
    unique.reserve(out.size());
    for (const auto& f : out)
    {
        if (seen.insert(f).second)
        {
            unique.push_back(f);
        }
    }
    return unique;
}

inline std::set<std::string> toFieldSet(const std::vector<std::string>& v)
{
    return std::set<std::string>(v.begin(), v.end());
}

inline std::vector<std::string> intersectionFields(const std::set<std::string>& a, const std::set<std::string>& b)
{
    std::vector<std::string> out;
    for (const auto& x : a)
    {
        if (b.count(x)) out.push_back(x);
    }
    return out;
}

} // namespace osp::server::utils




