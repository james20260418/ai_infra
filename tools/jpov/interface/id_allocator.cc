#include "tools/jpov/interface/id_allocator.h"

#include <glog/logging.h>

namespace jpov {

uint32_t IdAllocator::Acquire() {
    if (live_.size() + released_.size() >= (static_cast<std::size_t>(1) << 32) - 1) {
        // 理论上的 id 空间耗尽（live + released 挤满除 0 外的全部号）。实际不可达，
        // 但库不该静默回绕去踩 live —— 返回 0 让调用方按资源耗尽显式处理。
        LOG(WARNING) << "IdAllocator: id space exhausted, return 0";
        return 0;
    }

    uint32_t id = 0;
    if (!released_.empty()) {
        // 有空槽 → 复用最近释放的 id（LIFO，热槽先回）。
        id = released_.back();
        released_.pop_back();
    } else {
        // 无空槽 → 单调递增开新号，跳过无效 0（回绕时可能停在 0/已有值，见下 while）。
        while (next_id_ == 0 || live_.count(next_id_) != 0) {
            ++next_id_;
        }
        id = next_id_++;
    }
    live_.insert(id);
    return id;
}

void IdAllocator::Release(uint32_t id) {
    if (id == 0) {
        return;  // 0 无效。
    }
    auto it = live_.find(id);
    if (it == live_.end()) {
        return;  // 非 live（从未分配 / 已释放）→ 静默忽略，允许重复释放。
    }
    live_.erase(it);
    released_.push_back(id);  // 放回空槽池，供后续 Acquire 复用。
}

}  // namespace jpov
