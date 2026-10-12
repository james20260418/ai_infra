// JPOV 骨架 — 动作延迟（LAG / 副运动）CPU 工具（GL-free，header-only，可单测）。
//
// 背景（见 docs/jpov_instance_attr_lag_design.md）：布料 LAG 副运动 = 给实例送**两套 pose**
//   （当前 + 滞后），蒙皮 VS 按**逐顶点松弛度** aRelax 对两者 mix。设计 §4.2：pose_lag 是
//   **用户逐帧演进的状态**，接口对「怎么演进」透明 —— 纯延迟（历史回放）/ 一阶滤波 / 二阶都行。
//
// 本文件提供「怎么演进」这一半的三件工具（暴露给 JPOV 用户）：
//   ① SkeletonPoseLerp(a, b, t)：两个 SkeletonPose 的插值（逐骨 slerp 最短路径 + root_offset 线性）。
//   ② FirstOrderLagTracker：**一阶**（指数）延迟追踪，用「上升时间 rising_time」配置；无过冲。
//   ③ SecondOrderLagTracker：**二阶**（弹簧-阻尼）延迟追踪，用「超调量 overshoot + 上升时间」配置；
//      有摆动 / 过冲。
//
// 关键约定：
//   - 两套 tracker 都**逐关节**追踪（每根骨的 joint_rotation 各自滞后），并**把 root_offset 一并
//     追踪**（Danis：这类追踪器要考虑 root offset ⇒ 「滞后」是整姿态的概念，根位移也滞后）。
//   - 上升时间 rising_time 统一定义为阶跃响应的 **10%→90% 上升时间**（秒）。
//     · 一阶：τ = rising_time / ln(9)（10–90% 恰为 τ·ln9）。
//     · 二阶：ζ 由 overshoot 定，ω_n 取「使 10–90% 上升时间 == rising_time」（线性系统 rise time ∝ 1/ω，
//       故用 ω=1 的数值阶跃响应标定一次即可，见 UnitOmegaRiseTime）。
//   - 「时空矩阵」/ 摆放矩阵 / pos_lag 的延迟**不在此**（设计 §4.1：位置滞后只作用于平移列，
//     交用户自行演进）—— 本 util 只做 pose（关节旋转 + root 位移）。
//   - 确定性：逐帧显式积分（1 阶解析 / 2 阶半隐式欧拉 + 子步），同输入序列同输出。
//
// 职责边界：沿用 interface/(CPU/用户类型, GL-free) 约定，与 skeleton_types.h 同层。

#ifndef JPOV_INTERFACE_SKELETON_MOTION_LAG_H_
#define JPOV_INTERFACE_SKELETON_MOTION_LAG_H_

#include <algorithm>
#include <cmath>
#include <vector>

#include <glog/logging.h>
#include "geom/common/quaternion.h"
#include "geom/common/vec.h"
#include "tools/jpov/interface/skeleton_types.h"

namespace jpov {

// ==================== ① SkeletonPose 插值 ====================

// 两个 SkeletonPose 的插值：逐骨 joint_rotation 做 SLERP（自动走最短路径，见 geom::Slerp），
// root_offset 线性插值。t 会在 [0,1] 内 clamp。
// Pre-condition: a.bone_count == b.bone_count；a/b 的 joint_rotation 尺寸相等（允许同为空 =
//   隐式 rest，此时不动旋转只插 root_offset）。
inline SkeletonPose SkeletonPoseLerp(const SkeletonPose& a, const SkeletonPose& b, float t) {
    CHECK_EQ(a.bone_count, b.bone_count)
        << "SkeletonPoseLerp: 两个 pose 的 bone_count 不同（不同骨架的 pose 不能插值）";
    CHECK_EQ(a.joint_rotation.size(), b.joint_rotation.size())
        << "SkeletonPoseLerp: joint_rotation 尺寸不一致";
    const float tt = std::clamp(t, 0.0f, 1.0f);
    SkeletonPose r;
    r.bone_count = a.bone_count;
    r.joint_rotation.resize(a.joint_rotation.size());
    for (size_t i = 0; i < a.joint_rotation.size(); ++i) {
        r.joint_rotation[i] = geom::Slerp(a.joint_rotation[i], b.joint_rotation[i], tt);
    }
    r.root_offset = a.root_offset + (b.root_offset - a.root_offset) * tt;
    return r;
}

// ==================== ② 一阶延迟追踪 ====================

// 一阶（指数）动作延迟：无过冲、平滑拖尾。时间常数 τ 由「10%→90% 上升时间」定：τ = t_r / ln9。
//
// 用法（用户逐帧驱动）：
//   FirstOrderLagTracker t(0.08f);            // 上升时间 80ms
//   t.Reset(第一帧姿态);
//   每帧： t.Update(本帧目标姿态, dt);          // 输出 t.tracked() 作为 pose_lag
class FirstOrderLagTracker {
public:
    FirstOrderLagTracker() = default;
    explicit FirstOrderLagTracker(float rising_time_s) { Configure(rising_time_s); }

    // 配置上升时间（秒，> 0）。<= 0 → CHECK 崩溃（避免「静默不延迟」的假象）。
    void Configure(float rising_time_s) {
        CHECK_GT(rising_time_s, 0.0f)
            << "FirstOrderLagTracker::Configure: rising_time 必须 > 0";
        rising_time_s_ = rising_time_s;
        tau_s_ = rising_time_s / std::log(9.0f);  // 10%→90% = τ·ln9
    }

    // 用初始姿态设定滞后状态（通常给第一帧的目标姿态）。
    void Reset(const SkeletonPose& pose) {
        tracked_ = pose;
        initialized_ = true;
    }

    // 推进一帧：向 target 逼近。dt 单位秒（>= 0；0 = 不动）。首次调用等价 Reset(target)。
    void Update(const SkeletonPose& target, float dt) {
        CHECK_GT(rising_time_s_, 0.0f)
            << "FirstOrderLagTracker::Update: 未 Configure(rising_time)";
        CHECK_GE(dt, 0.0f) << "FirstOrderLagTracker::Update: dt 不能为负";
        if (!initialized_) {
            Reset(target);
            return;
        }
        if (dt <= 0.0f) {
            return;
        }
        const float alpha = 1.0f - std::exp(-dt / tau_s_);
        tracked_ = SkeletonPoseLerp(tracked_, target, alpha);
    }

    const SkeletonPose& tracked() const { return tracked_; }
    bool initialized() const { return initialized_; }
    float rising_time_s() const { return rising_time_s_; }

private:
    float rising_time_s_ = 0.0f;
    float tau_s_ = 0.0f;
    bool initialized_ = false;
    SkeletonPose tracked_;
};

// ==================== ③ 二阶延迟追踪 ====================

// 超调比例 overshoot（阶跃最大超出量，∈ [0,1)）→ 阻尼比 ζ。
//   M→0 ⇒ ζ→1（临界阻尼，无过冲）；M↑ ⇒ ζ↓（更弾、摆动更久）。
inline float OvershootToDampingRatio(float overshoot) {
    const float m = std::clamp(overshoot, 0.0f, 0.9f);
    if (m <= 1e-4f) {
        return 1.0f;  // 临界阻尼
    }
    const float lm = std::log(m);
    constexpr float kPi = 3.14159265358979323846f;
    return -lm / std::sqrt(kPi * kPi + lm * lm);
}

// 二阶（弹簧-阻尼）动作延迟内部的「ω=1 时 10%→90% 上升时间」标定：
//   标量阶跃响应 x'' = 1 − 2ζ·x' − x（半隐式欧拉）。线性系统 rise time ∝ 1/ω ⇒ 由它反推 ω_n。
inline float UnitOmegaRiseTime(float zeta) {
    double x = 0.0;
    double v = 0.0;
    const double h = 1e-3;              // ω=1：h 足够小
    const int kMaxSteps = 400000;       // 直到 t=400（ζ=1 时 ~5 到 0.99，绰绰有余）
    double t = 0.0;
    double t10 = -1.0;
    double t90 = -1.0;
    for (int k = 0; k < kMaxSteps; ++k) {
        const double a = 1.0 - 2.0 * static_cast<double>(zeta) * v - x;
        v += a * h;
        x += v * h;
        t += h;
        if (t10 < 0.0 && x >= 0.1) {
            t10 = t;
        }
        if (t10 >= 0.0 && t90 < 0.0 && x >= 0.9) {
            t90 = t;
            break;
        }
    }
    CHECK_GT(t90, t10) << "UnitOmegaRiseTime: 标定失败（zeta 非法？）";
    return static_cast<float>(t90 - t10);
}

// 二阶（弹簧-阻尼）动作延迟：有过冲与摆动。由「超调量 + 上升时间」两个物理量配置：
//   · ζ = OvershootToDampingRatio(overshoot)
//   · ω_n = UnitOmegaRiseTime(ζ) / rising_time（使 10–90% 上升时间 == rising_time）
//
// 用法（用户逐帧驱动，与一阶同构）：
//   SecondOrderLagTracker t(0.12f, 0.15f);    // 上升 120ms、超调 15%
//   t.Reset(第一帧姿态);
//   每帧： t.Update(本帧目标姿态, dt);          // 输出 t.tracked() 作为 pose_lag
//
// 说明：对每根骨，把「当前 tracked 旋转 → target 旋转」的**旋转向量**当作弹簧位移，
//   角速度按 ω'' = ω_n²·e − 2ζω_n·ω' 积分，再把增量旋转乘回 tracked；root_offset 同款线性弹簧。
class SecondOrderLagTracker {
public:
    SecondOrderLagTracker() = default;
    SecondOrderLagTracker(float rising_time_s, float overshoot) {
        Configure(rising_time_s, overshoot);
    }

    // rising_time_s > 0（秒，10%→90%）；overshoot ∈ [0,1)（0 = 临界阻尼）。
    void Configure(float rising_time_s, float overshoot) {
        CHECK_GT(rising_time_s, 0.0f)
            << "SecondOrderLagTracker::Configure: rising_time 必须 > 0";
        CHECK_GE(overshoot, 0.0f) << "SecondOrderLagTracker::Configure: overshoot 不能为负";
        rising_time_s_ = rising_time_s;
        overshoot_ = overshoot;
        zeta_ = OvershootToDampingRatio(overshoot);
        omega_ = UnitOmegaRiseTime(zeta_) / rising_time_s;
    }

    void Reset(const SkeletonPose& pose) {
        tracked_ = pose;
        omega_vec_.assign(tracked_.joint_rotation.size(), Vec3f(0.0f, 0.0f, 0.0f));
        root_vel_ = Vec3f(0.0f, 0.0f, 0.0f);
        initialized_ = true;
    }

    void Update(const SkeletonPose& target, float dt) {
        CHECK_GT(rising_time_s_, 0.0f)
            << "SecondOrderLagTracker::Update: 未 Configure(rising_time, overshoot)";
        CHECK_GE(dt, 0.0f) << "SecondOrderLagTracker::Update: dt 不能为负";
        if (!initialized_) {
            Reset(target);
            return;
        }
        CHECK_EQ(target.bone_count, tracked_.bone_count)
            << "SecondOrderLagTracker::Update: 目标姿态骨架不一致";
        if (dt <= 0.0f) {
            return;
        }
        const size_t nb = tracked_.joint_rotation.size();
        CHECK_EQ(target.joint_rotation.size(), nb);
        if (omega_vec_.size() != nb) {
            omega_vec_.assign(nb, Vec3f(0.0f, 0.0f, 0.0f));
        }
        // 稳定性：半隐式欧拉要求 ω_n·h 足够小，超了就子步。
        int substeps = 1;
        const float need = dt * omega_ / 0.1f;
        if (need > 1.0f) {
            substeps = static_cast<int>(std::ceil(need));
        }
        substeps = std::min(substeps, 200);
        const float h = dt / static_cast<float>(substeps);
        const float k = omega_ * omega_;
        const float c = 2.0f * zeta_ * omega_;
        for (int s = 0; s < substeps; ++s) {
            for (size_t i = 0; i < nb; ++i) {
                // 从 tracked 到 target 的旋转（父系）：qe = target · conj(tracked)，取最短路径。
                geom::Quaternion<float> qe =
                    target.joint_rotation[i] * tracked_.joint_rotation[i].Conjugate();
                if (qe.w < 0.0f) {
                    qe = geom::Quaternion<float>(-qe.x, -qe.y, -qe.z, -qe.w);
                }
                qe.NormalizeInPlace();
                // 旋转向量 e = 轴×角：e = 2·atan2(|v|, w)·v/|v|（|v|→0 时取 0）。
                Vec3f e(0.0f, 0.0f, 0.0f);
                {
                    Vec3f v(qe.x, qe.y, qe.z);
                    const float n2 = v.Sqr();
                    if (n2 > 1e-16f) {
                        const float n = std::sqrt(n2);
                        const float angle = 2.0f * std::atan2(n, qe.w);
                        e = v * (angle / n);
                    }
                }
                // 角速度：ω' += (k·e − c·ω)·h
                omega_vec_[i] += (e * k - omega_vec_[i] * c) * h;
                // 把增量旋转乘回：tracked = exp(ω·h) · tracked
                const Vec3f w = omega_vec_[i];
                const float wlen = std::sqrt(w.Sqr());
                if (wlen > 1e-9f) {
                    const geom::Quaternion<float> dq =
                        geom::Quaternion<float>::FromAxisAngle(w * (1.0f / wlen), wlen * h);
                    tracked_.joint_rotation[i] =
                        (dq * tracked_.joint_rotation[i]).Normalized();
                }
            }
            // root_offset：线性弹簧-阻尼。
            const Vec3f d = target.root_offset - tracked_.root_offset;
            root_vel_ += (d * k - root_vel_ * c) * h;
            tracked_.root_offset += root_vel_ * h;
        }
    }

    const SkeletonPose& tracked() const { return tracked_; }
    bool initialized() const { return initialized_; }
    float rising_time_s() const { return rising_time_s_; }
    float overshoot() const { return overshoot_; }
    float damping_ratio() const { return zeta_; }
    float natural_frequency() const { return omega_; }

private:
    float rising_time_s_ = 0.0f;
    float overshoot_ = 0.0f;
    float zeta_ = 1.0f;
    float omega_ = 0.0f;
    bool initialized_ = false;
    SkeletonPose tracked_;
    std::vector<Vec3f> omega_vec_;  // 每骨角速度（父系）
    Vec3f root_vel_{0.0f, 0.0f, 0.0f};
};

}  // namespace jpov

#endif  // JPOV_INTERFACE_SKELETON_MOTION_LAG_H_
