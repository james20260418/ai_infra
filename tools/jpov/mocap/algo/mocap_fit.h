// JPOV Mocap — 方向场 → Mixamo23 pose（含整体朝向 + 根位移 + 脚钉地）。
//
// 输入是 DirectionSequence（世界/相机系、Y-up 的单位骨方向），输出是 SkeletonPose 序列。
// 这里是**唯一的拟合实现点**：算法只需给方向，不需要知道骨架/四元数/root 语义。
//
// 数学（与 Python fit/mocap_fit.py、interface/skeleton_retarget.h 的偏差式同源）：
//
//   W(j) = Swing(Rb(j)·ŷ → d_body(j)) · Rb(j)      # 相对自己 rest 的最小摆动，零 twist
//   P(j) = B(j)⁻¹ · W(parent)⁻¹ · W(j)             # local 旋转（相对 bind）
//
// 其中 d_body = R_yaw⁻¹ · d_world（先把整体 yaw 摘掉，再由根骨统一施加），
// Rb(j) = 沿 bind 链复合出的 rest 世界朝向。
//
// **整体朝向（yaw）**：从髋线/肩线估（人体左右轴），用鼻尖-肩中心在水平面的方向
// 消除 ±180° 歧义。yaw 以 Hips 的 local 旋转承载，而不是包装 Root：
//   - Hips 在 FBX / glTF / BodyRetarget 里都有同名骨，能被下游真正搬走；
//   - 包装 Root 在多数资产里没有同名骨，写在那里等于「文件里有、蓝人身上没有」。
//   施加方式：P_hips ← (B_hips⁻¹ · R_yaw · B_hips) · P_hips（左乘世界轴 yaw 的共轭）。
//
// **根位移**：
//   - 水平 (x,z) = 髋中心 world 相对首帧的位移（米）。
//   - 竖直 y = 钉地量：先按 root_offset=0 做 FK，取最低的脚/趾关节 y，令
//     root_offset.y = −min_y，使最低点落在 y=0。**第一期假设至少一只脚着地**，
//     跳跃会被拉回地面（已知边界，见下）。
//   - 根位移做零相位平滑（无时间滞后），避免脚高度抖动带出「上下弹」。
//
// 已知边界（第一期，明确不装作支持）：
//   - 跳跃/腾空：没有接触检测，会被钉地逻辑压回地面。
//   - 根位移的深度 (z) 来自 MediaPipe world-z，仍然会漂；水平 x 也受其影响。
//   - 骨长比例固定 Mixamo23；被测者肢长差异会体现在肘/膝落点（设计文档 §6）。
//   - twist（绕骨轴）不可观测，swing-only 有意保持零 twist。

#ifndef JPOV_MOCAP_ALGO_MOCAP_FIT_H_
#define JPOV_MOCAP_ALGO_MOCAP_FIT_H_

#include <vector>

#include <glog/logging.h>
#include "geom/common/quaternion.h"
#include "geom/common/vec.h"
#include "tools/jpov/interface/skeleton_types.h"
#include "tools/jpov/mocap/algo/mocap_types.h"

namespace jpov {
namespace mocap {

using Quatf = geom::Quaternion<float>;

// 骨架空间下每关节的世界位置（含 pose.root_offset）。索引与骨架 joints 对齐。
// Pre-condition: skeleton.Validate() 通过；pose.bone_count == skeleton.bone_count()。
std::vector<Vec3f> ForwardKinematics(const SkeletonType& skeleton,
                                     const SkeletonPose& pose);

// 从一帧 track 观测估整体 yaw（弧度，绕 +Y，右手；0 = 模型系正面 +Z）。
//   - 髋线 + 肩线做圆周平均（肩线权重更高：肩比髋更少受腿型影响）；
//   - 鼻尖方向只用于在 yaw 与 yaw+π 之间选一个，不直接参与角度平均（鼻子噪声大）。
//   - world 缺失 / 退化时返回 0（不是 NaN），并在 fit 的诊断里保留求解状态。
float EstimateBodyYaw(const TrackFrame& frame);

// 方向场 → pose 序列。缺帧（valid=false）用最近有效方向顶替；整段都没有有效方向时
// 保留 rest（identity）并在日志里 WARNING。
//   track 参与两件事：估整体 yaw（需要 world 髋/肩/鼻）与取髋中心水平位移。
//   Pre-condition: track.frames.size() == directions.frames.size()。
FitSequence FitFromDirections(const SkeletonType& skeleton,
                              const DirectionSequence& directions,
                              const MocapTrack& track,
                              const FitOptions& options);

// 质量指标（用于批量比对；不参与拟合）。
// Pre-condition: track.frames.size() == sequence.frames.size()。
FitMetrics EvaluateFit(const MocapTrack& track, const SkeletonType& skeleton,
                       const FitSequence& sequence);

// 报告/调试用：把 yaw 归一到 (-π, π]。
float NormalizeAngleRad(float angle);

}  // namespace mocap
}  // namespace jpov

#endif  // JPOV_MOCAP_ALGO_MOCAP_FIT_H_
