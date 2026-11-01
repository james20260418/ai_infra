// JPOV WindowedStat — 「每 N 秒定格一次」的稳定读数（帧率 / 耗时 / 事件率）
//
// 为什么不用指数滑动平均（EMA）：
//   1. EMA 每帧都在变，屏幕上根本读不出来（debug HUD 的 fps 就吃过这个亏）；
//   2. 平滑强度随帧率漂移 —— 帧率越低越"粘"，同一份代码在不同机器上手感完全不同；
//   3. EMA 没有明确的统计区间，无法回答「这段时间到底多少帧」。
//
// 这里换成分窗统计：
//   · 一个窗口（默认 0.5s）内**累积**所有样本；
//   · 窗口跨过边界后的**下一次采样**触发结算（速率 / 均值 / 最小 / 最大 / 样本数），
//     结算用的分母是窗口的**实际**跨度（不是名义 0.5s）；
//   · 结算值在整个**下一个**窗口内保持不变 ⇒ 屏幕上的数字每 0.5s 才跳一次，
//     且每次跳的都是一个完整区间的统计结果（读得懂、可比、可复现）。
//
// 时间基准由调用方给（秒，单调不减）。JPOV 传 glfwGetTime()。
//
// 典型用法（帧率）：
//   stat.Tick(now);                        // 每帧一次
//   if (stat.has_result()) fps = stat.rate_per_second();
//
// 典型用法（模块耗时）：
//   stat.AddSample(ms, now);               // 每帧一次
//   if (stat.has_result()) hud = stat.mean();   // 平均耗时；还可看 max()

#ifndef JPOV_WINDOWED_STAT_H_
#define JPOV_WINDOWED_STAT_H_

#include <cstdint>

namespace jpov {

class WindowedStat {
public:
    // window_seconds：一个统计窗口的时长（秒）。
    // Pre-condition: window_seconds > 0
    explicit WindowedStat(double window_seconds = 0.5);

    // 记一次**事件**（帧 / 包 / 调用）：只计数，不参与 mean/min/max。
    // 返回 true 表示本次调用刚好关掉了一个窗口（结算已完成）。
    bool Tick(double now_seconds);

    // 记一个**带数值的样本**（如某模块耗时，毫秒）：计数 + 参与 mean/min/max。
    // 返回 true 表示本次调用刚好关掉了一个窗口。
    bool AddSample(double value, double now_seconds);

    // 是否已经有至少一个窗口结算过（第一个窗口结束之前为 false，此时读数无意义）。
    bool has_result() const { return has_result_; }

    // ---- 最近一个**已完成**窗口的结算值（在下一个窗口内保持不变）----
    // 事件/秒 = 窗口内事件数 / 窗口**实际**跨度（不是名义 0.5s ⇒ 卡顿/长间隔不会被
    // 平均掉）。窗口总是由一次采样触发关闭，所以 count 至少为 1。
    double rate_per_second() const { return rate_per_second_; }
    // 数值样本的均值 / 最小 / 最大。窗口内只有 Tick、没有 AddSample 时都是 0
    //（不沿用上一窗口：把两种性质的统计混在一起看更糟）。
    double mean() const { return mean_; }
    double min_value() const { return min_value_; }
    double max_value() const { return max_value_; }
    // 最近一个窗口里的事件总数（Tick + AddSample 都算一次事件）。
    int64_t count() const { return last_count_; }

private:
    // 累积一次计数；返回 true 表示跨过窗口边界并完成结算。
    bool Accumulate(double value, bool has_value, double now_seconds);
    void CloseWindow(double now_seconds);

    double window_seconds_;

    bool started_ = false;
    double window_start_ = 0.0;

    // 当前（未结算）窗口的累加器
    int64_t count_ = 0;
    int64_t value_count_ = 0;
    double sum_ = 0.0;
    double min_ = 0.0;
    double max_ = 0.0;

    // 最近一个已结算窗口的结果
    bool has_result_ = false;
    int64_t last_count_ = 0;
    double rate_per_second_ = 0.0;
    double mean_ = 0.0;
    double min_value_ = 0.0;
    double max_value_ = 0.0;
};

}  // namespace jpov

#endif  // JPOV_WINDOWED_STAT_H_
