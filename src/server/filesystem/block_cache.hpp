#pragma once

#include "common/logger.hpp"

#include <cstddef>
#include <list>
#include <unordered_map>
#include <vector>

namespace osp::fs
{

// 非持久化简化版 LRU 块缓存；后续可接入真实磁盘文件。
class BlockCache
{
public:
    explicit BlockCache(std::size_t capacity)
        : capacity_(capacity)
    {
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    std::vector<std::byte> get(std::size_t blockId, bool& hit)
    {
        auto it = map_.find(blockId);
        if (it == map_.end())
        {
            hit = false;
            osp::log(osp::LogLevel::Debug, "BlockCache miss");
            return {};
        }

        // 移动到 LRU 链表前端
        lru_.splice(lru_.begin(), lru_, it->second.lruIt);
        hit = true;
        osp::log(osp::LogLevel::Debug, "BlockCache hit");
        return it->second.data;
    }

    void put(std::size_t blockId, std::vector<std::byte> data)
    {
        auto it = map_.find(blockId);
        if (it != map_.end())
        {
            it->second.data = std::move(data);
            lru_.splice(lru_.begin(), lru_, it->second.lruIt);
            return;
        }

        if (map_.size() >= capacity_ && !lru_.empty())
        {
            auto victim = lru_.back();
            lru_.pop_back();
            map_.erase(victim);
            osp::log(osp::LogLevel::Debug, "BlockCache evict");
        }

        lru_.push_front(blockId);
        Entry e;
        e.data = std::move(data);
        e.lruIt = lru_.begin();
        map_.emplace(blockId, std::move(e));
    }

private:
    struct Entry
    {
        std::vector<std::byte> data;
        std::list<std::size_t>::iterator lruIt;
    };

    std::size_t capacity_;
    std::list<std::size_t> lru_;
    std::unordered_map<std::size_t, Entry> map_;
};

} // namespace osp::fs


