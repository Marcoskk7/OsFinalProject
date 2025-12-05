#include "vfs.hpp"

#include "common/logger.hpp"

namespace osp::fs
{

bool Vfs::mount(const std::string& backingFile)
{
    backingFile_ = backingFile;
    // 这里只是简单设置了一些字段，真实项目中应从磁盘文件读取 superblock。
    sb_.blockSize = 4096;
    sb_.totalBlocks = 1024;
    sb_.inodeTableStart = 1;
    sb_.dataBlockStart = 128;

    osp::log(osp::LogLevel::Info, "VFS mounted on " + backingFile_);
    return true;
}

std::optional<Inode> Vfs::createFile(const std::string& path)
{
    // 占位实现：仅输出日志，返回一个虚拟 inode
    osp::log(osp::LogLevel::Info, "createFile: " + path);
    Inode ino;
    ino.id = 1;
    ino.isDirectory = false;
    ino.size = 0;
    ino.firstBlock = sb_.dataBlockStart;
    return ino;
}

bool Vfs::removeFile(const std::string& path)
{
    osp::log(osp::LogLevel::Info, "removeFile: " + path);
    return true;
}

} // namespace osp::fs


