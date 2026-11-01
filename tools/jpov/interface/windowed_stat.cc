// JPOV WindowedStat 实现（纯 CPU，无 GL 依赖）。

#include "tools/jpov/interface/windowed_stat.h"

#include <glog/logging.h>

namespace jpov {

WindowedStat::WindowedStat(double window_seconds) : window_seconds_(window_seconds) {
    CHECK_GT(window_seconds_, 0.0) << "WindowedStat: window_seconds 必须 > 0";
}

bool WindowedStat::Tick(double now_seconds) {
    return Accumulate(0.0, /*has_value=*/false, now_seconds);
}

bool WindowedStat::AddSample(double value, double now_seconds) {
    return Accumulate(value, /*has_value=*/true, now_seconds);
}

bool WindowedStat::Accumulate(double value, bool has_value, double now_seconds) {
    if (!started_) {
        started_ = true;
        window_start_ = now_seconds;
    }
    // Pre-condition: 时间基准单调不减（JPOV 传 glfwGetTime，天然单调）。
    // 时钟回退说明调用方传错了时间源（墙钟/秒表混用），早崩早暴露。
    CHECK_GE(now_seconds, window_start_)
        << "WindowedStat: 时间基准必须单调不减（上次 " << window_start_
        << "s，本次 " << now_seconds << "s）";

    bool closed = false;
    if (now_seconds - window_start_ >= window_seconds_) {
        CloseWindow(now_seconds);
        // 新窗口从**关窗时刻**起算：本次采样计入新窗口。
        window_start_ = now_seconds;
        closed = true;
    }

    ++count_;
    if (has_value) {
        if (value_count_ == 0 || value < min_) {
            min_ = value;
        }
        if (value_count_ == 0 || value > max_) {
            max_ = value;
        }
        sum_ += value;
        ++value_count_;
    }
    return closed;
}

void WindowedStat::CloseWindow(double now_seconds) {
    const double elapsed = now_seconds - window_start_;
    rate_per_second_ =
        (elapsed > 0.0) ? static_cast<double>(count_) / elapsed : 0.0;
    if (value_count_ > 0) {
        mean_ = sum_ / static_cast<double>(value_count_);
        min_value_ = min_;
        max_value_ = max_;
    } else {
        // 只有 Tick 没有 AddSample：数值类读数保持 0（不是"沿用上一窗口"，
        // 免得把两个不同性质的统计混在一起看）。
        mean_ = 0.0;
        min_value_ = 0.0;
        max_value_ = 0.0;
    }
    last_count_ = count_;
    has_result_ = true;

    count_ = 0;
    value_count_ = 0;
    sum_ = 0.0;
    min_ = 0.0;
    max_ = 0.0;
}

}  // namespace jpov
