#include "vfs.hpp"

#include "common/logger.hpp"

#include <cassert>
#include <cstring>
#include <filesystem>

namespace osp::fs
{
namespace
{
constexpr std::uint32_t kFsMagic = 0x20251205;
}

bool Vfs::mount(const std::string& backingFile)
{
    backingFile_ = backingFile;

    namespace fs = std::filesystem;

    const bool existedBefore = fs::exists(backingFile_);

    // 以读写二进制方式打开，如果文件不存在则先创建再重新打开
    file_.open(backingFile_, std::ios::in | std::ios::out | std::ios::binary);
    if (!file_.is_open())
    {
        // 尝试创建新文件
        std::ofstream createFile(backingFile_, std::ios::out | std::ios::binary);
        if (!createFile.is_open())
        {
            osp::log(osp::LogLevel::Error, "VFS mount failed: cannot create backing file " + backingFile_);
            return false;
        }
        createFile.close();

        file_.open(backingFile_, std::ios::in | std::ios::out | std::ios::binary);
        if (!file_.is_open())
        {
            osp::log(osp::LogLevel::Error, "VFS mount failed: cannot reopen backing file " + backingFile_);
            return false;
        }
    }

    if (existedBefore && loadSuperBlock() && sb_.magic == kFsMagic)
    {
        osp::log(osp::LogLevel::Info, "VFS mounted existing filesystem on " + backingFile_);
        return true;
    }

    // 如果文件不存在，或者不是本项目格式，则重新格式化
    if (!formatNewFileSystem())
    {
        osp::log(osp::LogLevel::Error, "VFS mount failed: formatNewFileSystem() failed for " + backingFile_);
        return false;
    }

    osp::log(osp::LogLevel::Info, "VFS formatted and mounted on " + backingFile_);
    return true;
}

bool Vfs::loadSuperBlock()
{
    if (!file_.is_open())
    {
        return false;
    }

    file_.seekg(0, std::ios::beg);
    file_.read(reinterpret_cast<char*>(&sb_), static_cast<std::streamsize>(sizeof(SuperBlock)));
    if (!file_)
    {
        return false;
    }

    return true;
}

bool Vfs::flushSuperBlock()
{
    if (!file_.is_open())
    {
        return false;
    }

    file_.seekp(0, std::ios::beg);
    file_.write(reinterpret_cast<const char*>(&sb_), static_cast<std::streamsize>(sizeof(SuperBlock)));
    file_.flush();

    return static_cast<bool>(file_);
}

bool Vfs::formatNewFileSystem()
{
    if (!file_.is_open())
    {
        return false;
    }

    // 设定一个固定大小的磁盘布局，满足课程中“有 superblock/inode 表/数据块区域/空闲位图”的要求。
    constexpr std::uint32_t kBlockSize = 4096;
    constexpr std::uint32_t kTotalBlocks = 1024;
    constexpr std::uint32_t kInodeTableBlocks = 8;
    constexpr std::uint32_t kFreeBitmapBlocks = 1; // 4096*8=32768 bits，足够覆盖所有数据块

    sb_.magic = kFsMagic;
    sb_.blockSize = kBlockSize;
    sb_.totalBlocks = kTotalBlocks;

    sb_.inodeTableStart = 1;
    sb_.inodeTableBlocks = kInodeTableBlocks;

    const std::uint32_t inodesPerBlock =
        sb_.blockSize / static_cast<std::uint32_t>(sizeof(Inode));
    sb_.inodeCount = inodesPerBlock * sb_.inodeTableBlocks;

    sb_.freeBitmapStart = sb_.inodeTableStart + sb_.inodeTableBlocks;
    sb_.freeBitmapBlocks = kFreeBitmapBlocks;

    sb_.dataBlockStart = sb_.freeBitmapStart + sb_.freeBitmapBlocks;
    sb_.dataBlockCount = sb_.totalBlocks - sb_.dataBlockStart;

    sb_.rootInodeId = 0;

    // 调整 backing file 大小
    const std::uint64_t totalBytes =
        static_cast<std::uint64_t>(sb_.totalBlocks) * sb_.blockSize;

    file_.seekp(static_cast<std::streamoff>(totalBytes - 1), std::ios::beg);
    char zero = 0;
    file_.write(&zero, 1);
    file_.flush();

    if (!file_)
    {
        return false;
    }

    // 写入 superblock
    if (!flushSuperBlock())
    {
        return false;
    }

    // 准备一个全 0 的块缓冲区
    std::vector<std::byte> zeroBlock(sb_.blockSize, std::byte{0});

    // 清空 inode 表所在的块
    for (std::uint32_t i = 0; i < sb_.inodeTableBlocks; ++i)
    {
        if (!writeBlock(sb_.inodeTableStart + i, zeroBlock))
        {
            return false;
        }
    }

    // 初始化空闲位图为全 0（全部空闲）
    for (std::uint32_t i = 0; i < sb_.freeBitmapBlocks; ++i)
    {
        if (!writeBlock(sb_.freeBitmapStart + i, zeroBlock))
        {
            return false;
        }
    }

    // 创建根目录 inode，占用一个数据块
    std::uint32_t rootDataBlock{};
    if (!allocDataBlock(rootDataBlock))
    {
        return false;
    }

    Inode root{};
    root.id = sb_.rootInodeId;
    root.isDirectory = true;
    root.size = 0;
    for (std::size_t i = 0; i < Inode::MaxDirectBlocks; ++i)
    {
        root.directBlocks[i] = 0;
    }
    root.directBlocks[0] = rootDataBlock;

    if (!storeInode(root))
    {
        return false;
    }

    return true;
}

std::vector<std::byte> Vfs::readBlock(std::uint32_t blockId)
{
    bool hit = false;
    auto data = cache_.get(blockId, hit);
    if (hit)
    {
        return data;
    }

    if (!file_.is_open() || sb_.blockSize == 0)
    {
        return {};
    }

    data.assign(sb_.blockSize, std::byte{0});

    const auto offset =
        static_cast<std::streamoff>(blockId) * static_cast<std::streamoff>(sb_.blockSize);
    file_.seekg(offset, std::ios::beg);
    file_.read(reinterpret_cast<char*>(data.data()),
               static_cast<std::streamsize>(data.size()));

    if (!file_)
    {
        // 读失败时返回空向量
        return {};
    }

    cache_.put(blockId, data);
    return data;
}

bool Vfs::writeBlock(std::uint32_t blockId, const std::vector<std::byte>& data)
{
    if (!file_.is_open() || data.size() != sb_.blockSize)
    {
        return false;
    }

    const auto offset =
        static_cast<std::streamoff>(blockId) * static_cast<std::streamoff>(sb_.blockSize);
    file_.seekp(offset, std::ios::beg);
    file_.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
    file_.flush();

    if (!file_)
    {
        return false;
    }

    cache_.put(blockId, data);
    return true;
}

bool Vfs::loadInode(std::uint32_t id, Inode& out)
{
    if (sb_.blockSize == 0 || sb_.inodeTableBlocks == 0)
    {
        return false;
    }

    const std::uint32_t inodesPerBlock =
        sb_.blockSize / static_cast<std::uint32_t>(sizeof(Inode));
    if (inodesPerBlock == 0 || id >= sb_.inodeCount)
    {
        return false;
    }

    const std::uint32_t blockIndex = id / inodesPerBlock;
    const std::uint32_t indexInBlock = id % inodesPerBlock;
    if (blockIndex >= sb_.inodeTableBlocks)
    {
        return false;
    }

    const std::uint32_t blockId = sb_.inodeTableStart + blockIndex;
    auto block = readBlock(blockId);
    if (block.size() < sb_.blockSize)
    {
        return false;
    }

    const std::size_t offset =
        static_cast<std::size_t>(indexInBlock) * sizeof(Inode);
    if (offset + sizeof(Inode) > block.size())
    {
        return false;
    }

    std::memcpy(&out, block.data() + offset, sizeof(Inode));
    return true;
}

bool Vfs::storeInode(const Inode& ino)
{
    if (sb_.blockSize == 0 || sb_.inodeTableBlocks == 0)
    {
        return false;
    }

    const std::uint32_t inodesPerBlock =
        sb_.blockSize / static_cast<std::uint32_t>(sizeof(Inode));
    if (inodesPerBlock == 0 || ino.id >= sb_.inodeCount)
    {
        return false;
    }

    const std::uint32_t blockIndex = ino.id / inodesPerBlock;
    const std::uint32_t indexInBlock = ino.id % inodesPerBlock;
    if (blockIndex >= sb_.inodeTableBlocks)
    {
        return false;
    }

    const std::uint32_t blockId = sb_.inodeTableStart + blockIndex;
    auto block = readBlock(blockId);
    if (block.size() != sb_.blockSize)
    {
        // 不合法，重新分配一个空块
        block.assign(sb_.blockSize, std::byte{0});
    }

    const std::size_t offset =
        static_cast<std::size_t>(indexInBlock) * sizeof(Inode);
    if (offset + sizeof(Inode) > block.size())
    {
        return false;
    }

    std::memcpy(block.data() + offset, &ino, sizeof(Inode));

    return writeBlock(blockId, block);
}

bool Vfs::allocDataBlock(std::uint32_t& outBlockId)
{
    if (sb_.blockSize == 0 || sb_.freeBitmapBlocks == 0)
    {
        return false;
    }

    const std::uint32_t bitsPerBlock = sb_.blockSize * 8u;
    std::uint32_t remaining = sb_.dataBlockCount;
    std::uint32_t globalBitIndex = 0;

    for (std::uint32_t b = 0; b < sb_.freeBitmapBlocks && remaining > 0; ++b)
    {
        const std::uint32_t blockId = sb_.freeBitmapStart + b;
        auto bitmap = readBlock(blockId);
        if (bitmap.size() != sb_.blockSize)
        {
            return false;
        }

        const std::uint32_t bitsInThisBlock =
            std::min(bitsPerBlock, remaining);

        for (std::uint32_t bit = 0; bit < bitsInThisBlock; ++bit, ++globalBitIndex)
        {
            const std::uint32_t byteIndex = bit / 8u;
            const std::uint8_t bitMask = static_cast<std::uint8_t>(1u << (bit % 8u));

            auto& byteRef = reinterpret_cast<std::uint8_t&>(bitmap[byteIndex]);
            if ((byteRef & bitMask) == 0)
            {
                // 找到一个空闲块，标记为已占用
                byteRef |= bitMask;

                if (!writeBlock(blockId, bitmap))
                {
                    return false;
                }

                outBlockId = sb_.dataBlockStart + globalBitIndex;
                return true;
            }
        }

        remaining -= bitsInThisBlock;
    }

    return false; // 没有空闲数据块
}

bool Vfs::freeDataBlock(std::uint32_t blockId)
{
    if (sb_.blockSize == 0 || sb_.freeBitmapBlocks == 0)
    {
        return false;
    }

    if (blockId < sb_.dataBlockStart ||
        blockId >= sb_.dataBlockStart + sb_.dataBlockCount)
    {
        return false;
    }

    const std::uint32_t relative = blockId - sb_.dataBlockStart;
    const std::uint32_t bitsPerBlock = sb_.blockSize * 8u;

    const std::uint32_t bitmapBlockIndex = relative / bitsPerBlock;
    const std::uint32_t bitInBlock = relative % bitsPerBlock;
    if (bitmapBlockIndex >= sb_.freeBitmapBlocks)
    {
        return false;
    }

    const std::uint32_t bitmapBlockId = sb_.freeBitmapStart + bitmapBlockIndex;
    auto bitmap = readBlock(bitmapBlockId);
    if (bitmap.size() != sb_.blockSize)
    {
        return false;
    }

    const std::uint32_t byteIndex = bitInBlock / 8u;
    const std::uint8_t bitMask = static_cast<std::uint8_t>(1u << (bitInBlock % 8u));

    auto& byteRef = reinterpret_cast<std::uint8_t&>(bitmap[byteIndex]);
    byteRef &= static_cast<std::uint8_t>(~bitMask);

    return writeBlock(bitmapBlockId, bitmap);
}

bool Vfs::findFreeInode(std::uint32_t& outInodeId)
{
    if (sb_.inodeCount == 0)
    {
        return false;
    }

    // 从 1 开始，0 号留给根目录
    for (std::uint32_t id = 1; id < sb_.inodeCount; ++id)
    {
        Inode ino{};
        if (!loadInode(id, ino))
        {
            return false;
        }

        bool allZero = true;
        for (std::size_t i = 0; i < Inode::MaxDirectBlocks; ++i)
        {
            if (ino.directBlocks[i] != 0)
            {
                allZero = false;
                break;
            }
        }

        if (allZero && !ino.isDirectory && ino.size == 0)
        {
            outInodeId = id;
            return true;
        }
    }

    return false;
}

// ------------ 路径切分 ------------

bool Vfs::splitPath(const std::string& path, std::vector<std::string>& components) const
{
    components.clear();
    std::string current;

    for (char ch : path)
    {
        if (ch == '/')
        {
            if (!current.empty())
            {
                components.push_back(current);
                current.clear();
            }
        }
        else
        {
            current.push_back(ch);
        }
    }
    if (!current.empty())
    {
        components.push_back(current);
    }
    return true;
}

// ------------ 路径解析 ------------

bool Vfs::resolvePath(const std::string& path, std::uint32_t& outInodeId)
{
    if (path.empty() || path == "/")
    {
        outInodeId = sb_.rootInodeId;
        return true;
    }

    std::vector<std::string> comps;
    if (!splitPath(path, comps) || comps.empty())
    {
        return false;
    }

    std::uint32_t currentId = sb_.rootInodeId;
    Inode currentInode{};

    for (const auto& name : comps)
    {
        if (!loadInode(currentId, currentInode) || !currentInode.isDirectory)
        {
            return false;
        }

        std::vector<DirEntry> entries;
        if (!readDirectory(currentInode, entries))
        {
            return false;
        }

        bool found = false;
        for (const auto& e : entries)
        {
            if (e.inodeId != 0 && name == e.name)
            {
                currentId = e.inodeId;
                found = true;
                break;
            }
        }
        if (!found)
        {
            return false;
        }
    }

    outInodeId = currentId;
    return true;
}

bool Vfs::resolveParentDirectory(const std::string& path,
                                 std::uint32_t& outParentInodeId,
                                 std::string& outName)
{
    outName.clear();

    if (path.empty() || path == "/")
    {
        return false;
    }

    std::vector<std::string> comps;
    if (!splitPath(path, comps) || comps.empty())
    {
        return false;
    }

    outName = comps.back();
    comps.pop_back();

    if (outName.size() >= sizeof(DirEntry::name))
    {
        return false;
    }

    if (comps.empty())
    {
        outParentInodeId = sb_.rootInodeId;
        return true;
    }

    std::uint32_t currentId = sb_.rootInodeId;
    Inode currentInode{};

    for (const auto& name : comps)
    {
        if (!loadInode(currentId, currentInode) || !currentInode.isDirectory)
        {
            return false;
        }

        std::vector<DirEntry> entries;
        if (!readDirectory(currentInode, entries))
        {
            return false;
        }

        bool found = false;
        for (const auto& e : entries)
        {
            if (e.inodeId != 0 && name == e.name)
            {
                currentId = e.inodeId;
                found = true;
                break;
            }
        }
        if (!found)
        {
            return false;
        }
    }

    outParentInodeId = currentId;
    return true;
}

// ------------ 目录读写（单块目录） ------------

bool Vfs::readDirectory(const Inode& dirInode, std::vector<DirEntry>& entries)
{
    entries.clear();

    if (!dirInode.isDirectory)
    {
        return false;
    }

    if (dirInode.directBlocks[0] == 0)
    {
        return true;
    }

    auto block = readBlock(dirInode.directBlocks[0]);
    if (block.size() != sb_.blockSize)
    {
        return false;
    }

    const std::size_t maxEntries = sb_.blockSize / sizeof(DirEntry);
    for (std::size_t i = 0; i < maxEntries; ++i)
    {
        DirEntry e{};
        std::memcpy(&e, block.data() + i * sizeof(DirEntry), sizeof(DirEntry));
        if (e.inodeId != 0)
        {
            entries.push_back(e);
        }
    }
    return true;
}

bool Vfs::writeDirectory(Inode& dirInode, const std::vector<DirEntry>& entries)
{
    if (!dirInode.isDirectory)
    {
        return false;
    }

    if (dirInode.directBlocks[0] == 0)
    {
        if (!allocDataBlock(dirInode.directBlocks[0]))
        {
            return false;
        }
    }

    std::vector<std::byte> block(sb_.blockSize, std::byte{0});
    const std::size_t maxEntries = sb_.blockSize / sizeof(DirEntry);
    const std::size_t n = std::min(entries.size(), maxEntries);

    for (std::size_t i = 0; i < n; ++i)
    {
        std::memcpy(block.data() + i * sizeof(DirEntry),
                    &entries[i],
                    sizeof(DirEntry));
    }

    if (!writeBlock(dirInode.directBlocks[0], block))
    {
        return false;
    }

    dirInode.size = static_cast<std::uint32_t>(n * sizeof(DirEntry));
    return storeInode(dirInode);
}

// ------------ 高层接口实现：目录 / 文件 ------------

bool Vfs::createDirectory(const std::string& path)
{
    std::uint32_t parentId{};
    std::string name;
    if (!resolveParentDirectory(path, parentId, name))
    {
        return false;
    }

    Inode parent{};
    if (!loadInode(parentId, parent) || !parent.isDirectory)
    {
        return false;
    }

    std::vector<DirEntry> entries;
    if (!readDirectory(parent, entries))
    {
        return false;
    }
    for (const auto& e : entries)
    {
        if (e.inodeId != 0 && name == e.name)
        {
            return false;
        }
    }

    std::uint32_t inodeId{};
    if (!findFreeInode(inodeId))
    {
        return false;
    }

    std::uint32_t dataBlockId{};
    if (!allocDataBlock(dataBlockId))
    {
        return false;
    }

    Inode dir{};
    dir.id = inodeId;
    dir.isDirectory = true;
    dir.size = 0;
    for (std::size_t i = 0; i < Inode::MaxDirectBlocks; ++i)
    {
        dir.directBlocks[i] = 0;
    }
    dir.directBlocks[0] = dataBlockId;

    if (!storeInode(dir))
    {
        return false;
    }

    DirEntry e{};
    e.inodeId = inodeId;
    std::strncpy(e.name, name.c_str(), sizeof(e.name) - 1);
    e.name[sizeof(e.name) - 1] = '\0';

    entries.push_back(e);
    if (!writeDirectory(parent, entries))
    {
        return false;
    }

    return true;
}

std::optional<Inode> Vfs::createFile(const std::string& path)
{
    std::uint32_t parentId{};
    std::string name;
    if (!resolveParentDirectory(path, parentId, name))
    {
        osp::log(osp::LogLevel::Warn, "Vfs::createFile: resolveParentDirectory failed for " + path);
        return std::nullopt;
    }

    Inode parent{};
    if (!loadInode(parentId, parent) || !parent.isDirectory)
    {
        return std::nullopt;
    }

    std::vector<DirEntry> entries;
    if (!readDirectory(parent, entries))
    {
        return std::nullopt;
    }

    // 如果已存在同名条目，直接返回该 inode（如果是目录则认为失败）
    for (const auto& e : entries)
    {
        if (e.inodeId != 0 && name == e.name)
        {
            Inode existing{};
            if (!loadInode(e.inodeId, existing) || existing.isDirectory)
            {
                return std::nullopt;
            }
            return existing;
        }
    }

    std::uint32_t inodeId{};
    if (!findFreeInode(inodeId))
    {
        return std::nullopt;
    }

    std::uint32_t dataBlockId{};
    if (!allocDataBlock(dataBlockId))
    {
        return std::nullopt;
    }

    Inode ino{};
    ino.id = inodeId;
    ino.isDirectory = false;
    ino.size = 0;
    for (std::size_t i = 0; i < Inode::MaxDirectBlocks; ++i)
    {
        ino.directBlocks[i] = 0;
    }
    ino.directBlocks[0] = dataBlockId;

    if (!storeInode(ino))
    {
        return std::nullopt;
    }

    DirEntry e{};
    e.inodeId = inodeId;
    std::strncpy(e.name, name.c_str(), sizeof(e.name) - 1);
    e.name[sizeof(e.name) - 1] = '\0';

    entries.push_back(e);
    if (!writeDirectory(parent, entries))
    {
        return std::nullopt;
    }

    return ino;
}

bool Vfs::writeFile(const std::string& path, const std::string& data)
{
    auto maybeIno = createFile(path);
    if (!maybeIno)
    {
        return false;
    }
    Inode ino = *maybeIno;

    if (ino.isDirectory)
    {
        return false;
    }

    // 先释放原有数据块
    for (std::size_t i = 0; i < Inode::MaxDirectBlocks; ++i)
    {
        if (ino.directBlocks[i] != 0)
        {
            freeDataBlock(ino.directBlocks[i]);
            ino.directBlocks[i] = 0;
        }
    }

    const std::size_t totalSize = data.size();
    std::size_t offset = 0;
    std::size_t remaining = totalSize;

    for (std::size_t i = 0; i < Inode::MaxDirectBlocks && remaining > 0; ++i)
    {
        std::uint32_t blockId{};
        if (!allocDataBlock(blockId))
        {
            return false;
        }
        ino.directBlocks[i] = blockId;

        std::vector<std::byte> block(sb_.blockSize, std::byte{0});
        const std::size_t toCopy = std::min<std::size_t>(remaining, sb_.blockSize);

        std::memcpy(block.data(), data.data() + offset, toCopy);

        if (!writeBlock(blockId, block))
        {
            return false;
        }

        offset += toCopy;
        remaining -= toCopy;
    }

    if (remaining > 0)
    {
        // 文件太大，超出 MaxDirectBlocks * blockSize 能力
        return false;
    }

    ino.size = static_cast<std::uint32_t>(totalSize);
    return storeInode(ino);
}

std::optional<std::string> Vfs::readFile(const std::string& path)
{
    std::uint32_t inodeId{};
    if (!resolvePath(path, inodeId))
    {
        return std::nullopt;
    }

    Inode ino{};
    if (!loadInode(inodeId, ino) || ino.isDirectory)
    {
        return std::nullopt;
    }

    std::string result;
    result.resize(ino.size);

    std::size_t remaining = ino.size;
    std::size_t offset = 0;

    for (std::size_t i = 0; i < Inode::MaxDirectBlocks && remaining > 0; ++i)
    {
        if (ino.directBlocks[i] == 0)
        {
            break;
        }

        auto block = readBlock(ino.directBlocks[i]);
        if (block.size() != sb_.blockSize)
        {
            return std::nullopt;
        }

        const std::size_t toCopy = std::min<std::size_t>(remaining, sb_.blockSize);
        std::memcpy(result.data() + offset, block.data(), toCopy);

        offset += toCopy;
        remaining -= toCopy;
    }

    if (remaining != 0)
    {
        return std::nullopt;
    }

    return result;
}

bool Vfs::removeFile(const std::string& path)
{
    // 简化：只实现“删除普通文件 + 从父目录移除目录项”，不实现递归删除目录
    std::uint32_t inodeId{};
    if (!resolvePath(path, inodeId))
    {
        osp::log(osp::LogLevel::Info, "Vfs::removeFile: path not found: " + path);
        return false;
    }

    Inode ino{};
    if (!loadInode(inodeId, ino) || ino.isDirectory)
    {
        return false;
    }

    // 释放数据块
    for (std::size_t i = 0; i < Inode::MaxDirectBlocks; ++i)
    {
        if (ino.directBlocks[i] != 0)
        {
            freeDataBlock(ino.directBlocks[i]);
            ino.directBlocks[i] = 0;
        }
    }
    // 关键：将 inode 恢复为“空闲态”，便于 findFreeInode() 复用
    ino.isDirectory = false;
    ino.size = 0;
    storeInode(ino);

    // 从父目录中删掉目录项
    std::uint32_t parentId{};
    std::string name;
    if (!resolveParentDirectory(path, parentId, name))
    {
        return false;
    }

    Inode parent{};
    if (!loadInode(parentId, parent) || !parent.isDirectory)
    {
        return false;
    }

    std::vector<DirEntry> entries;
    if (!readDirectory(parent, entries))
    {
        return false;
    }

    bool erased = false;
    std::vector<DirEntry> newEntries;
    newEntries.reserve(entries.size());
    for (const auto& e : entries)
    {
        if (e.inodeId != 0 && name == e.name && !erased)
        {
            erased = true;
            continue;
        }
        newEntries.push_back(e);
    }

    if (!erased)
    {
        return false;
    }

    return writeDirectory(parent, newEntries);
}

bool Vfs::removeDirectory(const std::string& path)
{
    // 不允许删除根目录
    if (path.empty() || path == "/")
    {
        return false;
    }

    std::uint32_t inodeId{};
    if (!resolvePath(path, inodeId))
    {
        return false;
    }

    Inode dir{};
    if (!loadInode(inodeId, dir) || !dir.isDirectory)
    {
        return false;
    }

    // 只有空目录才允许删除
    std::vector<DirEntry> entries;
    if (!readDirectory(dir, entries))
    {
        return false;
    }
    for (const auto& e : entries)
    {
        if (e.inodeId != 0)
        {
            // 非空目录
            return false;
        }
    }

    // 释放目录占用的数据块
    for (std::size_t i = 0; i < Inode::MaxDirectBlocks; ++i)
    {
        if (dir.directBlocks[i] != 0)
        {
            freeDataBlock(dir.directBlocks[i]);
            dir.directBlocks[i] = 0;
        }
    }
    // 关键：将 inode 恢复为“空闲态”，否则目录 inode 会永久不可复用
    dir.isDirectory = false;
    dir.size = 0;
    storeInode(dir);

    // 从父目录中移除该目录的目录项
    std::uint32_t parentId{};
    std::string name;
    if (!resolveParentDirectory(path, parentId, name))
    {
        return false;
    }

    Inode parent{};
    if (!loadInode(parentId, parent) || !parent.isDirectory)
    {
        return false;
    }

    std::vector<DirEntry> parentEntries;
    if (!readDirectory(parent, parentEntries))
    {
        return false;
    }

    bool erased = false;
    std::vector<DirEntry> newEntries;
    newEntries.reserve(parentEntries.size());
    for (const auto& e : parentEntries)
    {
        if (e.inodeId != 0 && name == e.name && !erased)
        {
            erased = true;
            continue;
        }
        newEntries.push_back(e);
    }

    if (!erased)
    {
        return false;
    }

    return writeDirectory(parent, newEntries);
}

std::optional<std::string> Vfs::listDirectory(const std::string& path)
{
    std::uint32_t inodeId{};
    if (!resolvePath(path, inodeId))
    {
        return std::nullopt;
    }

    Inode ino{};
    if (!loadInode(inodeId, ino) || !ino.isDirectory)
    {
        return std::nullopt;
    }

    std::vector<DirEntry> entries;
    if (!readDirectory(ino, entries))
    {
        return std::nullopt;
    }

    std::string result;
    for (const auto& e : entries)
    {
        if (e.inodeId == 0)
        {
            continue;
        }

        Inode entryInode{};
        if (!loadInode(e.inodeId, entryInode))
        {
            continue;
        }

        if (!result.empty())
        {
            result.push_back('\n');
        }

        // 约定：目录名后追加 '/'，便于客户端区分文件与目录。
        result += e.name;
        if (entryInode.isDirectory)
        {
            result.push_back('/');
        }
    }

    return result;
}

} // namespace osp::fs


