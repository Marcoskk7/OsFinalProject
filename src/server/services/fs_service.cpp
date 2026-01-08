#include "fs_service.hpp"

#include <sstream>

namespace osp::server
{

std::optional<osp::protocol::Message> FsService::tryHandle(const osp::protocol::Command& cmd)
{
    using osp::protocol::json;

    if (cmd.name != "MKDIR" && cmd.name != "WRITE" && cmd.name != "READ" && cmd.name != "RM" && cmd.name != "RMDIR"
        && cmd.name != "LIST")
    {
        return std::nullopt;
    }

    if (cmd.name == "MKDIR")
    {
        if (cmd.args.empty())
        {
            return osp::protocol::makeErrorResponse("MISSING_ARGS", "MKDIR: missing path");
        }
        const std::string& path = cmd.args[0];

        bool ok;
        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            ok = vfs_.createDirectory(path);
        }

        if (ok)
        {
            return osp::protocol::makeSuccessResponse({{"message", "Directory created"}, {"path", path}});
        }
        return osp::protocol::makeErrorResponse("FS_ERROR", "MKDIR failed: " + path);
    }

    if (cmd.name == "WRITE")
    {
        if (cmd.rawArgs.empty())
        {
            return osp::protocol::makeErrorResponse("MISSING_ARGS", "WRITE: missing path");
        }

        std::istringstream iss(cmd.rawArgs);
        std::string        path;
        iss >> path;
        if (path.empty())
        {
            return osp::protocol::makeErrorResponse("MISSING_ARGS", "WRITE: missing path");
        }

        std::string content;
        std::getline(iss, content);
        if (!content.empty() && content.front() == ' ')
        {
            content.erase(content.begin());
        }

        bool ok;
        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            ok = vfs_.writeFile(path, content);
        }

        if (ok)
        {
            return osp::protocol::makeSuccessResponse({{"message", "File written"}, {"path", path}});
        }
        return osp::protocol::makeErrorResponse("FS_ERROR", "WRITE failed: " + path);
    }

    if (cmd.name == "READ")
    {
        if (cmd.args.empty())
        {
            return osp::protocol::makeErrorResponse("MISSING_ARGS", "READ: missing path");
        }
        const std::string& path = cmd.args[0];

        std::optional<std::string> data;
        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            data = vfs_.readFile(path);
        }

        if (!data)
        {
            return osp::protocol::makeErrorResponse("FS_ERROR", "READ failed: " + path);
        }
        return osp::protocol::makeSuccessResponse({{"path", path}, {"content", *data}});
    }

    if (cmd.name == "RM")
    {
        if (cmd.args.empty())
        {
            return osp::protocol::makeErrorResponse("MISSING_ARGS", "RM: missing path");
        }
        const std::string& path = cmd.args[0];

        bool ok;
        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            ok = vfs_.removeFile(path);
        }

        if (ok)
        {
            return osp::protocol::makeSuccessResponse({{"message", "File removed"}, {"path", path}});
        }
        return osp::protocol::makeErrorResponse("FS_ERROR", "RM failed: " + path);
    }

    if (cmd.name == "RMDIR")
    {
        if (cmd.args.empty())
        {
            return osp::protocol::makeErrorResponse("MISSING_ARGS", "RMDIR: missing path");
        }
        const std::string& path = cmd.args[0];

        bool ok;
        {
            std::lock_guard<std::mutex> lock(vfsMutex_);
            ok = vfs_.removeDirectory(path);
        }

        if (ok)
        {
            return osp::protocol::makeSuccessResponse({{"message", "Directory removed"}, {"path", path}});
        }
        return osp::protocol::makeErrorResponse("FS_ERROR", "RMDIR failed (maybe not empty?): " + path);
    }

    // LIST
    std::string path = "/";
    if (!cmd.args.empty())
    {
        path = cmd.args[0];
    }

    std::optional<std::string> listing;
    {
        std::lock_guard<std::mutex> lock(vfsMutex_);
        listing = vfs_.listDirectory(path);
    }

    if (!listing)
    {
        return osp::protocol::makeErrorResponse("FS_ERROR", "LIST failed: " + path);
    }

    std::stringstream ss(*listing);
    std::string       entry;
    json              entries = json::array();
    while (std::getline(ss, entry))
    {
        if (!entry.empty())
        {
            entries.push_back(entry);
        }
    }

    return osp::protocol::makeSuccessResponse({{"path", path}, {"entries", entries}});
}

} // namespace osp::server




