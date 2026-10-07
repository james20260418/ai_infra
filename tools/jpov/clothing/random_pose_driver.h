// JPOV 穿衣工具 — 「随机摆动测试」姿态发生器（纯函数 / 无 GL）
//
// 用途（2026-10-07 Danis 提）：在穿衣工具里**直接验证「衣服会不会跟着人体动」**——
//   让参考人体做一段**程序化随机动作**；蒙皮过的衣服绑在**同一份骨架实例**上，于是逐帧
//   跟随（零新增同步逻辑，同 fbx_viewer 的 --cloth_path 做法）。
//
// 为什么不走 Mixamo 动画资产：不同 Mixamo 源的 bind 姿态（T-pose / A-pose）、骨轴 roll、
//   层级都可能不同，导入外部动画**必须重定向**。而本方案在**目标骨架自己的关节局部空间**
//   里直接生成姿态（identity pose = 该骨架自己的 T-pose）⇒ **完全不涉及重定向**。
//
// 设计要点：
//   - **确定性**：每关节的转轴 / 初相 / 频次只由 (关节号, seed) 派生 ⇒ 主轴相位相同则姿态
//     逐位相同。「拖主轴 = 拖动画」可复现（无需保存任何播放状态）。
//   - **周期性**：各关节频次取**整数**（单位：周期 / 主轴周期）⇒ 相位 p 与 p+1 姿态完全一致；
//     播放循环无缝、主轴可无限累加后再按 1 周期取模。
//   - **限幅**：每关节摆角 = amplitude · sin(...) ⇒ |摆角| ≤ amplitude；amplitude = 0 →
//     全单位姿态（= 骨架的 bind / T-pose）。
//   - **根关节不摆**：只动非根关节，避免整具人体位移 / 翻转把衣服和取景甩出画面。
//
// 与渲染的接法：本文件只产出资产层 SkeletonPose（每关节旋转）。App 侧把一个主轴周期的姿态
//   **预烘焙**成序列交给 RegisterSkeleton（骨架注册表无释放接口 ⇒ 只注册一次；幅度作为
//   烘焙维度之一，见 clothing_tool_app.h），运行时在「同档位相邻两相位」间插值。
//
// 边界：关节数为 1（只有根）时返回全单位姿态（无处可摆）。

#ifndef JPOV_CLOTHING_RANDOM_POSE_DRIVER_H_
#define JPOV_CLOTHING_RANDOM_POSE_DRIVER_H_

#include <cmath>
#include <cstdint>

#include <glog/logging.h>

#include "geom/common/quaternion.h"
#include "geom/common/vec.h"
#include "tools/jpov/interface/skeleton_types.h"

namespace jpov {
namespace clothing {

inline constexpr float kMotionPi = 3.14159265358979323846f;

// 每关节每主轴周期的**最大循环次数**。取整数族 {1..kMotionMaxCycles} 保证周期无缝。
inline constexpr int kMotionMaxCycles = 3;

// 随机摆动参数（纯数据，无 GL）。
struct RandomMotionParams {
    // 每关节最大摆角（度）。0 = 不动（全单位姿态 = bind/T-pose）。
    float amplitude_deg = 30.0f;
    // 抖动风格种子：换一个值 = 换一套「每关节转轴 / 相位 / 频次」。
    int seed = 0;
};

// 一根骨的周期波：绕 axis 转 angle(phase) = amplitude · sin(2π · cycles · phase + phase_rad)。
struct JointWave {
    geom::Vec3<float> axis{0.0f, 1.0f, 0.0f};  // 单位转轴
    float phase_rad = 0.0f;                    // 初相（弧度）
    int cycles = 1;                            // 每主轴周期的整数循环数
};

// splitmix64：确定性整数哈希（与 grid_map.h 的混合思路同源；此处本地实现，不跨模块依赖）。
inline uint64_t MotionHash(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// 把 64 位哈希映射成 [0,1) 均匀浮点（取高 24 位）。结果严格 < 1。
inline float MotionUnit01(uint64_t x) {
    return static_cast<float>(x >> 40) / static_cast<float>(1ull << 24);
}

// 由 (joint_index, seed) 派生该骨的周期波（确定性；同输入恒同输出）。
inline JointWave ComputeJointWave(int joint_index, int seed) {
    // 先按 (seed, joint_index) 混一个根哈希，再派生 3 段相互独立的子哈希。
    const uint64_t root = MotionHash(
        (static_cast<uint64_t>(static_cast<uint32_t>(seed)) << 32) ^
        static_cast<uint64_t>(static_cast<uint32_t>(joint_index)));
    const uint64_t h_axis = MotionHash(root + 1ull);
    const uint64_t h_phase = MotionHash(root + 2ull);
    const uint64_t h_cycle = MotionHash(root + 3ull);

    JointWave wave;
    // 转轴：三个 [-1,1) 分量归一化；近零退化（概率极低）回退 +Y。
    const float ax = MotionUnit01(h_axis) * 2.0f - 1.0f;
    const float ay = MotionUnit01(MotionHash(h_axis + 1ull)) * 2.0f - 1.0f;
    const float az = MotionUnit01(MotionHash(h_axis + 2ull)) * 2.0f - 1.0f;
    const float len = std::sqrt(ax * ax + ay * ay + az * az);
    wave.axis = (len > 1e-4f)
                    ? geom::Vec3<float>(ax / len, ay / len, az / len)
                    : geom::Vec3<float>(0.0f, 1.0f, 0.0f);
    wave.phase_rad = MotionUnit01(h_phase) * 2.0f * kMotionPi;
    // MotionUnit01 ∈ [0,1) ⇒ floor(u·max) ∈ [0, max-1] ⇒ cycles ∈ [1, kMotionMaxCycles]。
    wave.cycles = 1 + static_cast<int>(MotionUnit01(h_cycle) *
                                       static_cast<float>(kMotionMaxCycles));
    return wave;
}

// 生成骨架在**主轴相位** phase（单位：周期；任意实数，内部按 1 周期取模）下的姿态。
//
// pose.joint_rotation[j] = FromAxisAngle(axis_j, amplitude·sin(2π·cycles_j·phase + phase_j))。
// 根关节（parent == kSkeletonNoParent）恒为单位旋转。
//
// Pre-condition: skeleton.bone_count() > 0；params.amplitude_deg >= 0。
inline jpov::SkeletonPose RandomPoseAt(const jpov::SkeletonType& skeleton,
                                       float phase,
                                       const RandomMotionParams& params) {
    const int bone_count = skeleton.bone_count();
    CHECK_GT(bone_count, 0) << "RandomPoseAt: 骨架无关节";
    CHECK_GE(params.amplitude_deg, 0.0f) << "RandomPoseAt: 幅度不能为负，got "
                                         << params.amplitude_deg;

    jpov::SkeletonPose pose = jpov::SkeletonPose::Identity(bone_count);
    const float amp_rad = params.amplitude_deg * kMotionPi / 180.0f;
    for (int j = 0; j < bone_count; ++j) {
        if (skeleton.joints[static_cast<size_t>(j)].parent == jpov::kSkeletonNoParent) {
            continue;  // 根关节不动（避免整具人体位移 / 翻转）
        }
        const JointWave wave = ComputeJointWave(j, params.seed);
        const float angle =
            amp_rad * std::sin(2.0f * kMotionPi * static_cast<float>(wave.cycles) *
                                   phase +
                               wave.phase_rad);
        pose.joint_rotation[static_cast<size_t>(j)] =
            geom::Quaternion<float>::FromAxisAngle(wave.axis, angle);
    }
    return pose;
}

}  // namespace clothing
}  // namespace jpov

#endif  // JPOV_CLOTHING_RANDOM_POSE_DRIVER_H_
