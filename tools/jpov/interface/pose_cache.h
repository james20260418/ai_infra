// JPOV — PoseCache：一个对象「主动作」的历史环形缓冲 + 纯时间延迟查询（GL-free, header-only, 可单测）。
//
// 背景：布料 LAG（docs/jpov_instance_attr_lag_design.md §4.2）要「滞后姿态」pose_lag。设计 §1.3
//   把**纯延迟（回放历史 pose）**列为首选基线：pose_lag = pose(now − delay)。渲染端 pose_lag 必须
//   是 **pose atlas 里的 pose**（SkeletonManager 构造即锁死、不能动态加 pose），而历史里存的
//   正是 {pose_a, pose_b, ratio} 这一「主动作三元组」——回放即取历史里更早的那一条。
//
// 本类做「一根手指的活」：把每帧的主动作 {pose_a, pose_b, ratio} 连同**时间戳 t** 按时间推进
//   压进一个**循环队列**；查询「delay 秒之前的那条」（**不做插值**——{a,b,ratio} 是 atlas 索引，
//   无法在索引空间插值；60Hz 下相邻采样足够密，直接取整条即可，2026-10-12 Danis 定）。
//
// 约定：
//   - **时间戳必须严格递增**：非递增（t <= 上次 t）的采样**直接忽略**（返回 false）。避免乱序 /
//     重复帧污染历史。
//   - 纯延迟查询 = 「不晚于 (now − delay) 的**最近**一条」；历史不够老（最早一条都晚于目标）时
//     返回**最早**那条（尽力而为，out_exact=false 告知）。
//   - 固定容量环形缓冲，覆盖最老样本；容量按「最大延迟 × 采样率」估（60Hz × 0.5s ≈ 32）。
//
// 一阶/二阶延迟「特效」（过冲 / 摆动）**不在本类**：设计约定这类连续化交给 per-instance 里能
//   动态改的维度（如 pos_lag / partial_lag）；本类只做纯延迟（见同目录 skeleton_motion_lag.h
//   的 tracker，是给这些连续维度的）。

#ifndef JPOV_INTERFACE_POSE_CACHE_H_
#define JPOV_INTERFACE_POSE_CACHE_H_

#include <algorithm>
#include <cstddef>
#include <vector>

#include <glog/logging.h>

namespace jpov {

// 一个采样：主动作三元组 {pose_a, pose_b, ratio} + 时间戳 t（秒，单调递增）。
struct PoseSample {
    int pose_a = 0;    // 插值起点：SkeletonManager pose 数组下标（0-based）。
    int pose_b = 0;    // 插值终点：同上。
    float ratio = 0.0f;  // [0,1] pose_a→pose_b 权重。
    float t = 0.0f;      // 采样时间戳（秒）。
};

// 一个对象的主动作历史（循环队列）+ 纯时间延迟查询。
class PoseCache {
public:
    // capacity >= 1（最多保留多少条历史）。容量按「最大延迟 × 采样率 + 余量」估。
    explicit PoseCache(size_t capacity) : cap_(capacity) {
        CHECK_GE(cap_, 1u) << "PoseCache: 容量必须 >= 1";
        ring_.resize(cap_);
    }

    // 采样：t 必须**严格递增**（<= 上次则忽略、返回 false）。ratio clamp 到 [0,1]。
    bool Push(int pose_a, int pose_b, float ratio, float t) {
        if (count_ > 0 && !(t > last_t_)) {
            return false;  // 非递增：忽略该采样
        }
        PoseSample& s = ring_[write_];
        s.pose_a = pose_a;
        s.pose_b = pose_b;
        s.ratio = std::clamp(ratio, 0.0f, 1.0f);
        s.t = t;
        write_ = (write_ + 1) % cap_;
        if (count_ < cap_) {
            ++count_;
        }
        last_t_ = t;
        return true;
    }
    bool Push(const PoseSample& s) { return Push(s.pose_a, s.pose_b, s.ratio, s.t); }

    // 纯时间延迟查询：返回「不晚于 (now − delay) 的最近一条采样」。
    //   历史不够老（最早一条都晚于目标时刻）→ 返回最早那条（尽力而为）。
    //   out_exact（可选）：是否命中（true = 确实存在 <= 目标的采样；false = 只能给最早那条）。
    // Pre-condition: 非空（空 → CHECK 崩溃；调用方应先判 empty()）。
    PoseSample QueryDelayed(float now, float delay, bool* out_exact = nullptr) const {
        CHECK_GT(count_, 0u) << "PoseCache::QueryDelayed: 缓存为空";
        const float target = now - delay;
        // 从**最新**往老遍历，取第一条 t <= target（时间递增 ⇒ 第一条满足的即「最近」）。
        // 环形索引：newest = write_-1；第 k 新 = write_-1-k。
        const size_t newest = (write_ + cap_ - 1) % cap_;
        for (size_t k = 0; k < count_; ++k) {
            const size_t idx = (newest + cap_ - k) % cap_;
            if (ring_[idx].t <= target) {
                if (out_exact != nullptr) {
                    *out_exact = true;
                }
                return ring_[idx];
            }
        }
        // 全部都晚于目标（历史太短）：给最早那条。
        const size_t oldest = (write_ + cap_ - count_) % cap_;
        if (out_exact != nullptr) {
            *out_exact = false;
        }
        return ring_[oldest];
    }

    size_t size() const { return count_; }
    size_t capacity() const { return cap_; }
    bool empty() const { return count_ == 0; }

    // 最新一条（Pre-condition: 非空）。
    const PoseSample& newest() const {
        CHECK_GT(count_, 0u) << "PoseCache::newest: 缓存为空";
        return ring_[(write_ + cap_ - 1) % cap_];
    }
    // 最老一条（Pre-condition: 非空）。
    const PoseSample& oldest() const {
        CHECK_GT(count_, 0u) << "PoseCache::oldest: 缓存为空";
        return ring_[(write_ + cap_ - count_) % cap_];
    }
    // 最近采样时间戳；空时返回 -inf。
    float last_time() const { return count_ == 0 ? -3.4e38f : last_t_; }

    void Clear() {
        count_ = 0;
        write_ = 0;
        last_t_ = 0.0f;
    }

private:
    std::vector<PoseSample> ring_;
    size_t cap_ = 0;
    size_t write_ = 0;    // 下一个写入位置（覆盖最老）。
    size_t count_ = 0;    // 当前条数（<= cap_）。
    float last_t_ = 0.0f; // 最近采样时间戳（单调递增校验用）。
};

}  // namespace jpov

#endif  // JPOV_INTERFACE_POSE_CACHE_H_
