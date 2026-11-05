// JPOV Mocap — 方向场 → Mixamo23 pose 的实现。
//
// 设计说明见 mocap_fit.h。这里刻意保持「一个函数只做一件事」：
//   RestRotations / Swing / SolveLocalPoses / ForwardKinematics / EstimateBodyYaw /
//   GroundRoot / EvaluateFit。批量调试时可以把中间量单独打出来，不需要猜。

#include "tools/jpov/mocap/algo/mocap_fit.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

#include <glog/logging.h>
#include "geom/common/quaternion.h"
#include "geom/common/vec.h"
#include "tools/jpov/mocap/algo/mocap_landmarks.h"
#include "tools/jpov/mocap/algo/mocap_math.h"

namespace jpov {
namespace mocap {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;

Quatf BindOf(const SkeletonType& skeleton, int joint) {
    if (skeleton.bind_rotation.empty()) {
        return Quatf::Identity();
    }
    return skeleton.bind_rotation[static_cast<size_t>(joint)];
}

// 每骨 rest 世界朝向：Rb(j) = Rb(parent) · B(j)。
std::vector<Quatf> RestWorldRotations(const SkeletonType& skeleton) {
    const size_t n = skeleton.joints.size();
    std::vector<Quatf> world(n);
    for (size_t j = 0; j < n; ++j) {
        const int p = skeleton.joints[j].parent;
        world[j] = ((p == kSkeletonNoParent) ? BindOf(skeleton, static_cast<int>(j))
                                             : world[static_cast<size_t>(p)] *
                                                   BindOf(skeleton, static_cast<int>(j)))
                       .Normalized();
    }
    return world;
}

// 最小旋转：把单位向量 a 转到单位向量 b。
// 反向（dot≈-1）时轴不唯一，取与 a 垂直的任一轴（与 Python 侧同款兜底）。
Quatf Swing(const Vec3f& a_raw, const Vec3f& b_raw) {
    Vec3f a = a_raw;
    Vec3f b = b_raw;
    CHECK(internal::NormalizeVec3(&a)) << "Swing: a 不能是零向量";
    CHECK(internal::NormalizeVec3(&b)) << "Swing: b 不能是零向量";
    const float d = std::max(-1.0f, std::min(1.0f, a.Dot(b)));
    if (d > 1.0f - 1e-9f) {
        return Quatf::Identity();
    }
    if (d < -1.0f + 1e-9f) {
        Vec3f axis{a.y(), -a.x(), 0.0f};
        if (axis.Norm() < 1e-6f) {
            axis = Vec3f{0.0f, -a.z(), a.y()};
        }
        CHECK(internal::NormalizeVec3(&axis));
        return Quatf::FromAxisAngle(axis, kPi);
    }
    const Vec3f cross = Vec3f{a.y() * b.z() - a.z() * b.y(),
                              a.z() * b.x() - a.x() * b.z(),
                              a.x() * b.y() - a.y() * b.x()};
    return Quatf(cross.x(), cross.y(), cross.z(), 1.0f + d).Normalized();
}

int FindJointBySuffixChecked(const SkeletonType& skeleton, const std::string& suffix) {
    for (size_t i = 0; i < skeleton.joints.size(); ++i) {
        const std::string& name = skeleton.joints[i].name;
        if (name.size() >= suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return static_cast<int>(i);
        }
    }
    LOG(FATAL) << "mocap: 骨架里找不到骨 \"" << suffix << "\"（Mixamo23 布局被破坏？）";
    return -1;
}

// 由目标方向（body-local）解每骨 local 旋转。
void SolveLocalPoses(const SkeletonType& skeleton,
                     const std::array<Vec3f, kMixamo23BoneCount>& dirs,
                     const std::array<bool, kMixamo23BoneCount>& valid,
                     std::vector<Quatf>* out) {
    CHECK(out != nullptr) << "SolveLocalPoses: out 不能为空";
    CHECK_EQ(skeleton.bone_count(), kMixamo23BoneCount);
    const std::vector<Quatf> rb = RestWorldRotations(skeleton);
    const size_t n = skeleton.joints.size();
    out->assign(n, Quatf::Identity());

    std::vector<Quatf> world(n, Quatf::Identity());
    world[0] = Quatf::Identity();
    for (size_t j = 1; j < n; ++j) {
        const int p = skeleton.joints[j].parent;
        CHECK_GE(p, 0);
        const Quatf bind = BindOf(skeleton, static_cast<int>(j));
        const Vec3f rest_dir = geom::RotateVector(rb[j], Vec3f{0.0f, 1.0f, 0.0f});
        Vec3f target = dirs[j];
        const bool usable = valid[j] && internal::NormalizeVec3(&target);
        world[j] = usable ? (Swing(rest_dir, target) * rb[j]).Normalized()
                          : (world[static_cast<size_t>(p)] * bind).Normalized();
        (*out)[j] = (BindOf(skeleton, static_cast<int>(j)).Conjugate() *
                     world[static_cast<size_t>(p)].Conjugate() * world[j])
                        .Normalized();
    }
}

std::vector<Quatf> ComputeWorldRotations(const SkeletonType& skeleton,
                                         const SkeletonPose& pose) {
    const size_t n = skeleton.joints.size();
    std::vector<Quatf> world(n, Quatf::Identity());
    for (size_t j = 0; j < n; ++j) {
        const Quatf bind = BindOf(skeleton, static_cast<int>(j));
        const Quatf local_pose =
            pose.joint_rotation.empty() ? Quatf::Identity() : pose.joint_rotation[j];
        const Quatf local = (bind * local_pose).Normalized();
        const int p = skeleton.joints[j].parent;
        world[j] = (p == kSkeletonNoParent) ? local
                                            : (world[static_cast<size_t>(p)] * local).Normalized();
    }
    return world;
}

// yaw 序列去卷绕后再平滑（直接平滑 ±π 边界会把 179° → -179° 当成跳变）。
std::vector<float> SmoothYaw(const std::vector<float>& yaw, int window) {
    if (yaw.empty()) {
        return yaw;
    }
    std::vector<float> unwrapped(yaw.size(), 0.0f);
    unwrapped[0] = yaw[0];
    for (size_t i = 1; i < yaw.size(); ++i) {
        float delta = NormalizeAngleRad(yaw[i] - yaw[i - 1]);
        unwrapped[i] = unwrapped[i - 1] + delta;
    }
    const std::vector<float> smoothed = internal::SmoothZeroPhase(unwrapped, window);
    std::vector<float> out(smoothed.size(), 0.0f);
    for (size_t i = 0; i < smoothed.size(); ++i) {
        out[i] = NormalizeAngleRad(smoothed[i]);
    }
    return out;
}

// 圆周平均：把若干 (angle, weight) 平均。权重为 0 / 退化时返回 fallback。
float CircularMean(const std::vector<std::pair<float, float>>& samples, float fallback) {
    float sx = 0.0f;
    float sy = 0.0f;
    float total = 0.0f;
    for (const auto& s : samples) {
        if (!(s.second > 0.0f)) {
            continue;
        }
        sx += s.second * std::cos(s.first);
        sy += s.second * std::sin(s.first);
        total += s.second;
    }
    if (total <= 0.0f || (std::abs(sx) + std::abs(sy)) < 1e-6f) {
        return fallback;
    }
    return std::atan2(sy, sx);
}

// 根位移平滑；同时把「平滑后脚仍穿地」的量算出来供指标使用。
std::array<std::vector<float>, 3> SmoothRoot(const FitSequence& sequence, int window) {
    std::array<std::vector<float>, 3> out;
    for (int axis = 0; axis < 3; ++axis) {
        std::vector<float> series(sequence.frames.size(), 0.0f);
        for (size_t i = 0; i < sequence.frames.size(); ++i) {
            series[i] = sequence.frames[i].pose.root_offset.m[axis];
        }
        out[static_cast<size_t>(axis)] = internal::SmoothZeroPhase(series, window);
    }
    return out;
}

float MinFootY(const SkeletonType& skeleton, const std::vector<Vec3f>& positions) {
    const int lf = FindJointBySuffixChecked(skeleton, "LeftFoot");
    const int rf = FindJointBySuffixChecked(skeleton, "RightFoot");
    const int lt = FindJointBySuffixChecked(skeleton, "LeftToeBase");
    const int rt = FindJointBySuffixChecked(skeleton, "RightToeBase");
    float min_y = std::numeric_limits<float>::max();
    for (int idx : {lf, rf, lt, rt}) {
        min_y = std::min(min_y, positions[static_cast<size_t>(idx)].y());
    }
    CHECK(std::isfinite(min_y)) << "MinFootY: 没算到脚关节位置";
    return min_y;
}

}  // namespace

float NormalizeAngleRad(float angle) {
    angle = std::fmod(angle + kPi, kTwoPi);
    if (angle < 0.0f) {
        angle += kTwoPi;
    }
    return angle - kPi;
}

std::vector<Vec3f> ForwardKinematics(const SkeletonType& skeleton,
                                     const SkeletonPose& pose) {
    skeleton.Validate();
    CHECK_EQ(pose.bone_count, skeleton.bone_count())
        << "ForwardKinematics: pose.bone_count(" << pose.bone_count
        << ") 必须 == skeleton.bone_count(" << skeleton.bone_count() << ")";
    const std::vector<Quatf> world = ComputeWorldRotations(skeleton, pose);
    const size_t n = skeleton.joints.size();
    std::vector<Vec3f> positions(n, Vec3f{});
    for (size_t j = 0; j < n; ++j) {
        const int p = skeleton.joints[j].parent;
        Vec3f offset = skeleton.joints[j].rest_offset;
        if (p == kSkeletonNoParent) {
            offset = Vec3f{offset.x() + pose.root_offset.x(),
                           offset.y() + pose.root_offset.y(),
                           offset.z() + pose.root_offset.z()};
            positions[j] = offset;
        } else {
            const Quatf& parent_rot = world[static_cast<size_t>(p)];
            positions[j] = positions[static_cast<size_t>(p)] + geom::RotateVector(parent_rot, offset);
        }
    }
    return positions;
}

float EstimateBodyYaw(const TrackFrame& frame) {
    if (!frame.has_world) {
        return 0.0f;
    }
    Vec3f left_hip;
    Vec3f right_hip;
    Vec3f left_shoulder;
    Vec3f right_shoulder;
    Vec3f nose;
    if (!Point3D(frame, kLeftHip, &left_hip) || !Point3D(frame, kRightHip, &right_hip) ||
        !Point3D(frame, kLeftShoulder, &left_shoulder) ||
        !Point3D(frame, kRightShoulder, &right_shoulder) ||
        !Point3D(frame, kNose, &nose)) {
        return 0.0f;
    }

    // 髋线：左髋 − 右髋（模型 +X = 左）。投影到水平面（y=0）。
    Vec3f hip_side{left_hip.x() - right_hip.x(), 0.0f, left_hip.z() - right_hip.z()};
    Vec3f shoulder_side{left_shoulder.x() - right_shoulder.x(), 0.0f,
                        left_shoulder.z() - right_shoulder.z()};

    std::vector<std::pair<float, float>> samples;
    if (hip_side.Norm() > 0.02f) {
        samples.emplace_back(std::atan2(-hip_side.z(), hip_side.x()), 0.4f);
    }
    if (shoulder_side.Norm() > 0.02f) {
        samples.emplace_back(std::atan2(-shoulder_side.z(), shoulder_side.x()), 0.6f);
    }
    float yaw = CircularMean(samples, 0.0f);

    // 鼻尖-肩中心：只用于消除「面朝 +Z 还是 −Z」的二义性。
    Vec3f shoulder_center{0.5f * (left_shoulder.x() + right_shoulder.x()), 0.0f,
                          0.5f * (left_shoulder.z() + right_shoulder.z())};
    Vec3f face{nose.x() - shoulder_center.x(), 0.0f, nose.z() - shoulder_center.z()};
    if (face.Norm() > 0.02f) {
        const float face_yaw = std::atan2(face.x(), face.z());
        const float d0 = std::abs(NormalizeAngleRad(face_yaw - yaw));
        const float d1 = std::abs(NormalizeAngleRad(face_yaw - (yaw + kPi)));
        if (d1 < d0) {
            yaw += kPi;
        }
    }
    return NormalizeAngleRad(yaw);
}

FitSequence FitFromDirections(const SkeletonType& skeleton,
                              const DirectionSequence& directions,
                              const MocapTrack& track,
                              const FitOptions& options) {
    skeleton.Validate();
    CHECK_EQ(skeleton.bone_count(), kMixamo23BoneCount);
    CHECK_GT(options.subject_height_m, 0.0f)
        << "FitFromDirections: subject_height_m 必须 >0";
    CHECK_EQ(track.frames.size(), directions.frames.size())
        << "FitFromDirections: track 与 directions 帧数不一致";

    const size_t frame_count = directions.frames.size();
    FitSequence sequence;
    sequence.fps = 0.0;  // 由调用方按 track.fps 填（拟合本身不需要时间）。
    sequence.subject_height_m = options.subject_height_m;
    sequence.frames.resize(frame_count);

    // ── 1. 整体 yaw：逐帧估 + 去卷绕平滑 ──────────────────────────────────
    std::vector<float> raw_yaw(frame_count, 0.0f);
    if (options.solve_body_yaw) {
        for (size_t i = 0; i < frame_count; ++i) {
            raw_yaw[i] = EstimateBodyYaw(track.frames[i]);
        }
    }
    // 平滑窗口与根位移共用（<=1 不平滑）。
    const int window = std::max(1, options.root_smoothing_window);
    const std::vector<float> yaw = SmoothYaw(raw_yaw, window);

    // ── 2. 缺骨的最近有效方向补齐（只用于拟合，不改 valid 报告）────────────
    std::vector<std::array<Vec3f, kMixamo23BoneCount>> filled(frame_count);
    std::vector<std::array<bool, kMixamo23BoneCount>> filled_valid(frame_count);
    for (size_t i = 0; i < frame_count; ++i) {
        filled[i] = directions.frames[i].direction;
        filled_valid[i] = directions.frames[i].valid;
    }
    for (int j = 1; j < kMixamo23BoneCount; ++j) {
        int first = -1;
        for (size_t i = 0; i < frame_count; ++i) {
            if (filled_valid[i][static_cast<size_t>(j)]) {
                first = static_cast<int>(i);
                break;
            }
        }
        if (first < 0) {
            continue;  // 整段都没观测到该骨：保持 invalid，调用方回落到 rest
        }
        int last = first;
        for (size_t i = 0; i < frame_count; ++i) {
            if (filled_valid[i][static_cast<size_t>(j)]) {
                last = static_cast<int>(i);
            } else {
                filled[i][static_cast<size_t>(j)] =
                    filled[static_cast<size_t>(last)][static_cast<size_t>(j)];
            }
        }
        int next = last;
        for (size_t r = frame_count; r > 0; --r) {
            const size_t i = r - 1;
            if (filled_valid[i][static_cast<size_t>(j)]) {
                next = static_cast<int>(i);
            } else if (static_cast<int>(i) < first) {
                filled[i][static_cast<size_t>(j)] =
                    filled[static_cast<size_t>(next)][static_cast<size_t>(j)];
            }
        }
        // 补过的帧现在有可用方向：让 SolveLocalPoses 真正使用它（而不是又回落 rest）。
        for (size_t i = 0; i < frame_count; ++i) {
            filled_valid[i][static_cast<size_t>(j)] = true;
        }
    }

    const int hips = FindJointBySuffixChecked(skeleton, "Hips");
    const Quatf hips_bind = BindOf(skeleton, hips);

    // ── 3. 逐帧求解 ────────────────────────────────────────────────────────
    std::vector<Vec3f> hip_world(frame_count, Vec3f{});
    std::vector<bool> hip_valid(frame_count, false);
    for (size_t i = 0; i < frame_count; ++i) {
        std::array<Vec3f, kMixamo23BoneCount> body_dirs = filled[i];
        const float yaw_i = yaw[i];
        const Quatf q_yaw = Quatf::FromAxisAngle(Vec3f{0.0f, 1.0f, 0.0f}, yaw_i);
        const Quatf q_yaw_inv = q_yaw.Conjugate();
        for (int j = 1; j < kMixamo23BoneCount; ++j) {
            if (!filled_valid[i][static_cast<size_t>(j)]) {
                continue;
            }
            body_dirs[static_cast<size_t>(j)] =
                geom::RotateVector(q_yaw_inv, body_dirs[static_cast<size_t>(j)]);
        }
        std::vector<Quatf> local;
        SolveLocalPoses(skeleton, body_dirs, filled_valid[i], &local);

        // yaw 落在 Hips：P_hips ← (B⁻¹ · Y · B) · P_hips
        const Quatf yaw_on_hips =
            (hips_bind.Conjugate() * q_yaw * hips_bind * local[static_cast<size_t>(hips)])
                .Normalized();
        local[static_cast<size_t>(hips)] = yaw_on_hips;

        SkeletonPose pose = SkeletonPose::Identity(skeleton.bone_count());
        pose.joint_rotation = std::move(local);
        pose.root_offset = Vec3f{};

        // 脚最低点（root_offset=0 时），用于钉地。
        std::vector<Vec3f> positions = ForwardKinematics(skeleton, pose);
        const float min_foot_y = MinFootY(skeleton, positions);
        sequence.frames[i].pose = std::move(pose);
        sequence.frames[i].body_yaw_rad = yaw_i;
        sequence.frames[i].foot_clearance_m = min_foot_y;

        // 髋中心位移（原始 world，Y-up）。
        Vec3f hip;
        if (Point3D(track.frames[i], kHipCenter, &hip)) {
            hip_world[i] = hip;
            hip_valid[i] = true;
        }
    }

    // ── 4. 根位移（水平 + 钉地）─────────────────────────────────────────────
    if (options.solve_root_translation) {
        Vec3f origin{};
        bool has_origin = false;
        for (size_t i = 0; i < frame_count; ++i) {
            if (hip_valid[i]) {
                origin = hip_world[i];
                has_origin = true;
                break;
            }
        }
        if (!has_origin) {
            LOG(WARNING) << "FitFromDirections: 没有任何帧带 world 髋点，root 水平位移置 0";
        }
        for (size_t i = 0; i < frame_count; ++i) {
            Vec3f root{0.0f, 0.0f, 0.0f};
            if (has_origin && hip_valid[i]) {
                const Vec3f delta = hip_world[i] - origin;
                root.m[0] = delta.x();
                root.m[2] = delta.z();
            }
            if (options.ground_feet) {
                // foot_clearance 是 root=0 时的最低点；钉地后该点回到 0。
                root.m[1] = -sequence.frames[i].foot_clearance_m;
            }
            sequence.frames[i].pose.root_offset = root;
        }
    }

    // ── 5. 根位移零相位平滑 ────────────────────────────────────────────────
    if (options.root_smoothing_window > 1 && !sequence.frames.empty()) {
        const std::array<std::vector<float>, 3> smoothed =
            SmoothRoot(sequence, options.root_smoothing_window);
        for (size_t i = 0; i < sequence.frames.size(); ++i) {
            for (int axis = 0; axis < 3; ++axis) {
                sequence.frames[i].pose.root_offset.m[axis] =
                    smoothed[static_cast<size_t>(axis)][i];
            }
            if (options.ground_feet) {
                // 平滑会把个别帧的 root.y 压低 -> 脚穿地。取 max(平滑值, 钉地所需值)：
                // 只在会穿地的帧抬起，不破坏其余帧的平滑（实测穿地从 18mm -> 0）。
                const float required_y = -sequence.frames[i].foot_clearance_m;
                sequence.frames[i].pose.root_offset.m[1] =
                    std::max(sequence.frames[i].pose.root_offset.m[1], required_y);
            }
        }
    }
    return sequence;
}

FitMetrics EvaluateFit(const MocapTrack& track, const SkeletonType& skeleton,
                       const FitSequence& sequence) {
    CHECK_EQ(track.frames.size(), sequence.frames.size())
        << "EvaluateFit: track/sequence 帧数不一致";
    FitMetrics metrics;
    const size_t n = skeleton.joints.size();
    std::vector<std::vector<Vec3f>> positions(track.frames.size());
    double error_sum = 0.0;
    int error_count = 0;
    float root_min = std::numeric_limits<float>::max();
    float root_max = std::numeric_limits<float>::lowest();
    float penetration = 0.0f;

    for (size_t i = 0; i < track.frames.size(); ++i) {
        const SkeletonPose& pose = sequence.frames[i].pose;
        positions[i] = ForwardKinematics(skeleton, pose);
        const std::vector<Quatf> world = ComputeWorldRotations(skeleton, pose);

        root_min = std::min(root_min, pose.root_offset.y());
        root_max = std::max(root_max, pose.root_offset.y());
        const float min_foot = MinFootY(skeleton, positions[i]);
        penetration = std::max(penetration, -min_foot);

        geom::Vec2<float> observed;
        for (int j = 1; j < kMixamo23BoneCount; ++j) {
            if (!RawBoneVector2D(track.frames[i], j, &observed)) {
                continue;
            }
            if (observed.Norm() < 8.0f) {
                continue;
            }
            const Vec3f fk_dir = geom::RotateVector(world[static_cast<size_t>(j)],
                                                    Vec3f{0.0f, 1.0f, 0.0f});
            const float fk_len = std::sqrt(fk_dir.x() * fk_dir.x() + fk_dir.y() * fk_dir.y());
            if (fk_len < 1e-6f) {
                continue;
            }
            const float ox = observed.x() / observed.Norm();
            const float oy = observed.y() / observed.Norm();
            const float fx = fk_dir.x() / fk_len;
            const float fy = fk_dir.y() / fk_len;
            const float dot = std::abs(ox * fx + oy * fy);
            error_sum += std::acos(internal::Clamp(dot, -1.0f, 1.0f)) * 180.0f / kPi;
            ++error_count;
        }
    }
    metrics.evaluated_bones = error_count;
    metrics.mean_direction_error_deg =
        error_count > 0 ? static_cast<float>(error_sum / error_count) : 0.0f;
    metrics.root_y_min_m = root_min == std::numeric_limits<float>::max() ? 0.0f : root_min;
    metrics.root_y_max_m = root_max == std::numeric_limits<float>::lowest() ? 0.0f : root_max;
    metrics.max_foot_penetration_m = penetration;

    // 抖动：关节位置二阶差分 RMS（米/帧²）。帧少于 3 时无定义 → 0。
    if (positions.size() >= 3) {
        double accel_sum = 0.0;
        int accel_count = 0;
        for (size_t i = 2; i < positions.size(); ++i) {
            for (size_t j = 0; j < n; ++j) {
                const Vec3f accel = positions[i][j] - positions[i - 1][j] * 2.0f + positions[i - 2][j];
                accel_sum += static_cast<double>(accel.Sqr());
                ++accel_count;
            }
        }
        metrics.jitter_m = accel_count > 0
                               ? static_cast<float>(std::sqrt(accel_sum / accel_count))
                               : 0.0f;
    }
    return metrics;
}

}  // namespace mocap
}  // namespace jpov
