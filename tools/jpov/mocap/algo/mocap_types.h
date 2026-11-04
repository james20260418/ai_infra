// JPOV Mocap — 视频动捕（track → Mixamo23 pose）的 CPU 数据契约。
//
// 这一层只定义「外部提取器给了什么」与「拟合算法之间传什么」，不读文件、不碰 GL。
// 目录 `tools/jpov/mocap/algo/` 是**动捕算法**的独立落点（Danis 2026-11-04 定）：
// 第一期先把多种算法收进同一套接口，便于批量比对，而不是把某一种算法焊死在链路里。
//
// 坐标系铁律（与 Python 侧 extract/pose_extract.py + fit/mocap_fit.py 完全一致）：
//   - MediaPipe 原始 landmarks：像素空间，y **向下**；world：米制近似值，y **向下**，
//     z **越小越靠近相机**。
//   - 本文件的 MocapTrack 在**加载边界**统一翻成 **Y-up、+Z 朝相机**：
//       2D: (x, -y) 像素，原点在左上角，y 向上；
//       3D: (x, -y, -z) 米，y 向上、+Z 朝相机。
//   下游算法只面对 Y-up，不再各自翻一次（翻两次 = 动作上下颠倒，真踩过）。
//
// 长度单位：world / pose / root_offset 全部是**米**。骨架由 Mixamo23Skeleton(height)
//   按被测者身高生成；track 的 world 是 MediaPipe 的近似米制，二者可直接比较。
//
// 缺失帧：提取器可能给 `landmarks: []` / `world: []`。本契约用 has_landmarks /
//   has_world 显式表达「这一帧没有」，**不做插值**——补帧在拟合侧做，且必须留下痕迹
//   （见 mocap_fit.cc 的 FillMissingFrames）。

#ifndef JPOV_MOCAP_ALGO_MOCAP_TYPES_H_
#define JPOV_MOCAP_ALGO_MOCAP_TYPES_H_

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <glog/logging.h>
#include "geom/common/vec.h"
#include "tools/jpov/interface/mixamo23_skeleton.h"

namespace jpov {
namespace mocap {

using Vec3f = geom::Vec3<float>;

// MediaPipe PoseLandmarker 的 33 点索引（与 extract/pose_extract.py 同名）。
inline constexpr int kLandmarkCount = 33;
inline constexpr int kNose = 0;
inline constexpr int kLeftEar = 7;
inline constexpr int kRightEar = 8;
inline constexpr int kLeftShoulder = 11;
inline constexpr int kRightShoulder = 12;
inline constexpr int kLeftElbow = 13;
inline constexpr int kRightElbow = 14;
inline constexpr int kLeftWrist = 15;
inline constexpr int kRightWrist = 16;
inline constexpr int kLeftPinky = 17;
inline constexpr int kRightPinky = 18;
inline constexpr int kLeftIndex = 19;
inline constexpr int kRightIndex = 20;
inline constexpr int kLeftHip = 23;
inline constexpr int kRightHip = 24;
inline constexpr int kLeftKnee = 25;
inline constexpr int kRightKnee = 26;
inline constexpr int kLeftAnkle = 27;
inline constexpr int kRightAnkle = 28;
inline constexpr int kLeftHeel = 29;
inline constexpr int kRightHeel = 30;
inline constexpr int kLeftFoot = 31;
inline constexpr int kRightFoot = 32;

// 2D 关键点（像素，已翻成 y-up）。z 只用于调试，方向计算不读它。
struct Landmark2D {
    float x_px = 0.0f;
    float y_px = 0.0f;
    float z_rel = 0.0f;
    float visibility = 0.0f;
};

// 一帧原始观测。
struct TrackFrame {
    double t_seconds = 0.0;
    float presence = 0.0f;
    bool has_landmarks = false;
    bool has_world = false;
    std::array<Landmark2D, kLandmarkCount> landmarks{};
    std::array<Vec3f, kLandmarkCount> world{};  // 米，Y-up
};

// 一段视频的逐帧观测（提取器的输出契约）。
struct MocapTrack {
    int version = 0;
    double fps = 0.0;
    int width = 0;
    int height = 0;
    std::string source;
    std::vector<TrackFrame> frames;
};

// 一根骨的目标方向（Mixamo23 的索引；index 0 = 包装 Root，恒不使用）。
//   direction 是**世界/相机系**（Y-up）下的单位向量；valid=false 表示这一帧这根骨
//   没有可信方向（遮挡、投影过短、算法退化），拟合侧必须回落到 rest 朝向。
struct BoneDirectionFrame {
    std::array<Vec3f, kMixamo23BoneCount> direction{};
    std::array<bool, kMixamo23BoneCount> valid{};
};

// 一种方向场算法的完整输出。
//   diagnostics 只用于批量比对/排障，不参与拟合：
//     px_per_meter —— 该段视频的全局像素/米（lift2d 族用；worldz 也可填，便于报告）；
//     bone_length_m —— 每骨长度先验（中位数；拟合/评价不直接读，但报告要看）。
struct DirectionSequence {
    std::string algorithm_name;
    std::vector<BoneDirectionFrame> frames;
    float px_per_meter = 0.0f;
    std::array<float, kMixamo23BoneCount> bone_length_m{};
};

// 拟合选项。默认值是「站立动作最安全的组合」：解整体朝向、解根位移、脚踩地、轻平滑。
struct FitOptions {
    // 被测者骨骼身高（米，脚底到头顶）。用于生成 Mixamo23 骨架；不是骨架自带高度。
    float subject_height_m = 1.75f;

    // 是否从髋/肩线估整体朝向（绕世界 Y 的 yaw）。关掉 = 保持模型系正面。
    bool solve_body_yaw = true;

    // 是否把髋中心的水平位移搬成 root_offset(x,z)。
    bool solve_root_translation = true;

    // 是否把最低脚关节钉到 y=0（消除「悬空」）。
    // ⚠️ 第一期假设**至少一只脚接触地面**；跳跃动作会被拉回地面。见 mocap_fit.cc 注释。
    bool ground_feet = true;

    // 根位移的零相位平滑窗口（帧）。<=1 = 不平滑。
    int root_smoothing_window = 5;
};

// 一帧拟合结果 + 诊断量。
struct FitFrame {
    SkeletonPose pose;         // joint_rotation 已含整体 yaw（落在 Hips 上），root_offset 已填。
    float body_yaw_rad = 0.0f; // 估出来的整体朝向（0 = 模型系正面 +Z；右手绕 +Y）。
    float foot_clearance_m = 0.0f;  // 钉地**前**最低脚关节的 y（正 = 悬空量）。
};

struct FitSequence {
    double fps = 0.0;
    float subject_height_m = 0.0f;
    std::vector<FitFrame> frames;
};

// 批量比对用的质量指标。
struct FitMetrics {
    float mean_direction_error_deg = 0.0f;  // FK 骨方向 vs 画面 2D 方向（只量方向，避比例差）
    float jitter_m = 0.0f;                  // 关节位置二阶差分 RMS（米/帧²）
    float root_y_min_m = 0.0f;
    float root_y_max_m = 0.0f;
    float max_foot_penetration_m = 0.0f;    // 钉地后仍低于 y=0 的最大深度（正数）
    int evaluated_bones = 0;                // 参与 dir_err 的样本数（0 = 指标无效）
};

}  // namespace mocap
}  // namespace jpov

#endif  // JPOV_MOCAP_ALGO_MOCAP_TYPES_H_
