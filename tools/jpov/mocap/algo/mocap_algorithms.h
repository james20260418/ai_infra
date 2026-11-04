// JPOV Mocap — 骨方向场算法注册表（GL-free）。
//
// 设计目标（Danis 2026-11-04）：**第一期多收集算法**，用同一套 fit/评价跑批量比对。
// 因此算法与「方向 → pose」解耦：
//
//   track.json ──[DirectionAlgorithm]──▶ 每帧 22 根骨的世界方向
//                                       └─[FitFromDirections]──▶ Mixamo23 pose
//
// 加一种新算法 = 在 mocap_algorithms.cc 里写一个 Compute… 函数并注册到
// ListDirectionAlgorithms()，**不改 fit、不改 CLI、不改评价**。
//
// 现有算法：
//   worldz    直接信 MediaPipe world landmarks（网络回归的近似 3D）。
//   worldz_t  worldz + 零相位时间平滑（把 world 的深度噪声压下去，作为时序基线）。
//   lift2d    只用 2D 关键点 + 骨长先验反解深度：投影长度 u 已知、骨长 L 已知
//             ⇒ 出平面分量 sqrt(L²−u²)；符号借 world-z 差值。画面内方向精确。
//   lift2d_t  lift2d + 2D 零相位平滑 + 深度符号中值保持 + 深度再平滑。抖动最低。
//
// 所有算法输出同一个 DirectionSequence：方向是**单位向量、世界/相机系、Y-up**。
// 算法侧**不做**整体朝向归一（yaw 由 fit 统一估、统一施加），否则「算法差异」会
// 和「朝向处理差异」混在一起，批量比对就失去意义。

#ifndef JPOV_MOCAP_ALGO_MOCAP_ALGORITHMS_H_
#define JPOV_MOCAP_ALGO_MOCAP_ALGORITHMS_H_

#include <functional>
#include <string>
#include <vector>

#include "tools/jpov/interface/skeleton_types.h"
#include "tools/jpov/mocap/algo/mocap_types.h"

namespace jpov {
namespace mocap {

// 一种方向场算法。
//   compute 的输入：track + 目标骨架（用于骨长先验的 fallback）；
//   输出必须与 track.frames 等长（缺帧用 invalid 表达，不跳号）。
struct DirectionAlgorithm {
    std::string name;         // CLI / 报告里的稳定标识（小写下划线）
    std::string description;  // 一句话说明（报告/`--list` 用）
    std::function<DirectionSequence(const MocapTrack&, const SkeletonType&)> compute;
};

// 全量注册表（顺序即 CLI 默认报告顺序）。
const std::vector<DirectionAlgorithm>& ListDirectionAlgorithms();

// 按 name 查；找不到返回 nullptr（调用方必须显式处理，不 fallback 到第一个）。
const DirectionAlgorithm* FindDirectionAlgorithm(const std::string& name);

// 由骨段表 + 目标骨架的 rest_offset 取每骨长度先验（米）。
// 用于 lift2d 的 L：优先用 track 内 world 长度的中位数（被测者自己的比例），
// 缺 world 的骨才回落到骨架 rest_offset 长度。
std::array<float, kMixamo23BoneCount> EstimateBoneLengths(
    const MocapTrack& track, const SkeletonType& skeleton);

// 全局像素/米：跨帧「所有非手/趾段的 2D 总长 / 3D 总长」的中位数。
// 单独暴露是为了批量报告能看出「这段视频的尺度估计是否离谱」。
float EstimatePixelsPerMeter(const MocapTrack& track);

}  // namespace mocap
}  // namespace jpov

#endif  // JPOV_MOCAP_ALGO_MOCAP_ALGORITHMS_H_
