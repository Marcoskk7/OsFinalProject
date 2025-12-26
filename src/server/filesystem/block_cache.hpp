#pragma once

#include "common/logger.hpp"

#include <cstddef>
#include <list>
#include <utility>
#include <unordered_map>
#include <vector>

namespace osp::fs
{

// 非持久化简化版 LRU 块缓存；后续可接入真实磁盘文件。
class BlockCache
{
public:
    struct Stats
    {
        std::size_t hits{0};
        std::size_t misses{0};
        std::size_t replacements{0}; // evictions
        std::size_t entries{0};
        std::size_t capacity{0};
    };

    explicit BlockCache(std::size_t capacity)
        : capacity_(capacity)
    {
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t size() const noexcept { return map_.size(); }

    [[nodiscard]] Stats stats() const noexcept
    {
        Stats s;
        s.hits = hits_;
        s.misses = misses_;
        s.replacements = replacements_;
        s.entries = map_.size();
        s.capacity = capacity_;
        return s;
    }

    void resetStats() noexcept
    {
        hits_ = 0;
        misses_ = 0;
        replacements_ = 0;
    }

    std::vector<std::byte> get(std::size_t blockId, bool& hit)
    {
        if (capacity_ == 0)
        {
            hit = false;
            ++misses_;
            return {};
        }

        auto it = map_.find(blockId);
        if (it == map_.end())
        {
            hit = false;
            ++misses_;
            osp::log(osp::LogLevel::Debug, "BlockCache miss");
            return {};
        }

        // 移动到 LRU 链表前端
        lru_.splice(lru_.begin(), lru_, it->second.lruIt);
        hit = true;
        ++hits_;
        osp::log(osp::LogLevel::Debug, "BlockCache hit");
        return it->second.data;
    }

    void put(std::size_t blockId, std::vector<std::byte> data)
    {
        if (capacity_ == 0)
        {
            // cache disabled
            return;
        }

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
            ++replacements_;
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

    std::size_t hits_{0};
    std::size_t misses_{0};
    std::size_t replacements_{0};
};

} // namespace osp::fs


