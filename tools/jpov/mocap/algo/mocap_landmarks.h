// JPOV Mocap — 关键点 → 骨段 的几何表（GL-free）。
//
// 为什么单独一个文件：算法（worldz / lift2d / …）与评价指标都必须用**同一张**骨段表。
// 表只写一份，换算法时不会出现「A 算法用左肘、B 算法用右肘」这种静默分叉。
//
// 骨段语义：
//   - 骨方向 = point(child) − point(parent)，单位向量。
//   - 虚拟点（负索引）与 Python fit/mocap_fit.py 逐字同义：
//       -1 = 髋中心，-2 = 肩中心，-3 = 双耳中心，-5 = 头顶，-6/-7 = 左/右手。
//   - Mixamo23 的 Hips/Spine/Spine1/Spine2 四根骨都取「髋中心 → 肩中心」的同一方向：
//     33 点里没有脊椎中段，硬编 4 个不同方向只会制造虚假精度（设计文档 §6 已记录）。
//
// 索引 0（包装 Root）不参与：它没有语义方向，恒 zero + invalid。

#ifndef JPOV_MOCAP_ALGO_MOCAP_LANDMARKS_H_
#define JPOV_MOCAP_ALGO_MOCAP_LANDMARKS_H_

#include <array>
#include <string>

#include <glog/logging.h>
#include "geom/common/vec.h"
#include "tools/jpov/mocap/algo/mocap_types.h"

namespace jpov {
namespace mocap {

// 虚拟点哨兵（与 Python 侧同名）。
inline constexpr int kHipCenter = -1;
inline constexpr int kShoulderCenter = -2;
inline constexpr int kEarCenter = -3;
inline constexpr int kHeadTop = -5;
inline constexpr int kLeftHandTip = -6;
inline constexpr int kRightHandTip = -7;

// 一根骨在 Mixamo23 下标 → (parent landmark, child landmark)。
struct BoneSegment {
    const char* joint_name;  // 与 SkeletonType::joints[].name 对得上（含 mixamorig: 前缀）
    int parent_landmark;
    int child_landmark;
};

// 22 根具名骨的段表（下标 = Mixamo23 的 idx 1..22，顺序与 mixamo23.py 的 SEG 一致）。
// 使用 std::array<BoneSegment, 23> 是为了让下标与骨架骨号**一一对应**：
//   kSegments[0] = {"", 0, 0}（Root 占位，永不读取）。
inline constexpr std::array<BoneSegment, kMixamo23BoneCount> kSegments = {{
    {"", 0, 0},
    {"mixamorig:Hips", kHipCenter, kShoulderCenter},
    {"mixamorig:Spine", kHipCenter, kShoulderCenter},
    {"mixamorig:Spine1", kHipCenter, kShoulderCenter},
    {"mixamorig:Spine2", kHipCenter, kShoulderCenter},
    {"mixamorig:Neck", kShoulderCenter, kEarCenter},
    {"mixamorig:Head", kEarCenter, kHeadTop},
    {"mixamorig:LeftShoulder", kShoulderCenter, kLeftShoulder},
    {"mixamorig:LeftArm", kLeftShoulder, kLeftElbow},
    {"mixamorig:LeftForeArm", kLeftElbow, kLeftWrist},
    {"mixamorig:LeftHand", kLeftWrist, kLeftHandTip},
    {"mixamorig:RightShoulder", kShoulderCenter, kRightShoulder},
    {"mixamorig:RightArm", kRightShoulder, kRightElbow},
    {"mixamorig:RightForeArm", kRightElbow, kRightWrist},
    {"mixamorig:RightHand", kRightWrist, kRightHandTip},
    {"mixamorig:LeftUpLeg", kLeftHip, kLeftKnee},
    {"mixamorig:LeftLeg", kLeftKnee, kLeftAnkle},
    {"mixamorig:LeftFoot", kLeftAnkle, kLeftFoot},
    {"mixamorig:LeftToeBase", kLeftHeel, kLeftFoot},
    {"mixamorig:RightUpLeg", kRightHip, kRightKnee},
    {"mixamorig:RightLeg", kRightKnee, kRightAnkle},
    {"mixamorig:RightFoot", kRightAnkle, kRightFoot},
    {"mixamorig:RightToeBase", kRightHeel, kRightFoot},
}};

// 2D 观测下的一个点（像素，y-up）。缺失点返回 false，不写 NaN 到 out。
// Pre-condition: landmark 在 {-7..32}；范围外的值直接 CHECK 崩（表写错了要当场发现）。
inline bool Point2D(const TrackFrame& frame, int landmark, geom::Vec2<float>* out) {
    CHECK(out != nullptr) << "Point2D: out 不能为空";
    CHECK(landmark >= kRightHandTip && landmark < kLandmarkCount)
        << "Point2D: landmark 索引越界: " << landmark;
    if (!frame.has_landmarks) {
        return false;
    }
    const auto& p = frame.landmarks;
    auto avg = [&p](int a, int b) {
        return geom::Vec2<float>{0.5f * (p[static_cast<size_t>(a)].x_px +
                                        p[static_cast<size_t>(b)].x_px),
                                 0.5f * (p[static_cast<size_t>(a)].y_px +
                                         p[static_cast<size_t>(b)].y_px)};
    };
    switch (landmark) {
        case kHipCenter:
            *out = avg(kLeftHip, kRightHip);
            return true;
        case kShoulderCenter:
            *out = avg(kLeftShoulder, kRightShoulder);
            return true;
        case kEarCenter:
            *out = avg(kLeftEar, kRightEar);
            return true;
        case kHeadTop: {
            const geom::Vec2<float> hc = avg(kLeftEar, kRightEar);
            const geom::Vec2<float> sc = avg(kLeftShoulder, kRightShoulder);
            *out = geom::Vec2<float>{hc.x() + (hc.x() - sc.x()) * 0.8f,
                                     hc.y() + (hc.y() - sc.y()) * 0.8f};
            return true;
        }
        case kLeftHandTip:
            *out = avg(kLeftIndex, kLeftPinky);
            return true;
        case kRightHandTip:
            *out = avg(kRightIndex, kRightPinky);
            return true;
        default: {
            const auto& q = p[static_cast<size_t>(landmark)];
            *out = geom::Vec2<float>{q.x_px, q.y_px};
            return true;
        }
    }
}

// 3D 观测下的一个点（米，Y-up）。语义与 Point2D 完全平行。
inline bool Point3D(const TrackFrame& frame, int landmark, Vec3f* out) {
    CHECK(out != nullptr) << "Point3D: out 不能为空";
    CHECK(landmark >= kRightHandTip && landmark < kLandmarkCount)
        << "Point3D: landmark 索引越界: " << landmark;
    if (!frame.has_world) {
        return false;
    }
    const auto& p = frame.world;
    auto avg = [&p](int a, int b) {
        return Vec3f{0.5f * (p[static_cast<size_t>(a)].x() + p[static_cast<size_t>(b)].x()),
                     0.5f * (p[static_cast<size_t>(a)].y() + p[static_cast<size_t>(b)].y()),
                     0.5f * (p[static_cast<size_t>(a)].z() + p[static_cast<size_t>(b)].z())};
    };
    switch (landmark) {
        case kHipCenter:
            *out = avg(kLeftHip, kRightHip);
            return true;
        case kShoulderCenter:
            *out = avg(kLeftShoulder, kRightShoulder);
            return true;
        case kEarCenter:
            *out = avg(kLeftEar, kRightEar);
            return true;
        case kHeadTop: {
            const Vec3f hc = avg(kLeftEar, kRightEar);
            const Vec3f sc = avg(kLeftShoulder, kRightShoulder);
            *out = Vec3f{hc.x() + (hc.x() - sc.x()) * 0.8f,
                         hc.y() + (hc.y() - sc.y()) * 0.8f,
                         hc.z() + (hc.z() - sc.z()) * 0.8f};
            return true;
        }
        case kLeftHandTip:
            *out = avg(kLeftIndex, kLeftPinky);
            return true;
        case kRightHandTip:
            *out = avg(kRightIndex, kRightPinky);
            return true;
        default: {
            *out = p[static_cast<size_t>(landmark)];
            return true;
        }
    }
}

// 由 TrackFrame 直接算一根骨的方向（未归一化）。返回 false = 该骨这一帧不可用。
inline bool RawBoneVector3D(const TrackFrame& frame, int bone_index, Vec3f* out) {
    CHECK_GE(bone_index, 0);
    CHECK_LT(bone_index, kMixamo23BoneCount);
    CHECK(out != nullptr) << "RawBoneVector3D: out 不能为空";
    const BoneSegment& seg = kSegments[static_cast<size_t>(bone_index)];
    if (bone_index == 0) {
        return false;
    }
    Vec3f parent;
    Vec3f child;
    if (!Point3D(frame, seg.parent_landmark, &parent) ||
        !Point3D(frame, seg.child_landmark, &child)) {
        return false;
    }
    *out = child - parent;
    return out->IsFinite();
}

// 2D 版本（像素，y-up）。
inline bool RawBoneVector2D(const TrackFrame& frame, int bone_index,
                            geom::Vec2<float>* out) {
    CHECK_GE(bone_index, 0);
    CHECK_LT(bone_index, kMixamo23BoneCount);
    CHECK(out != nullptr) << "RawBoneVector2D: out 不能为空";
    if (bone_index == 0) {
        return false;
    }
    const BoneSegment& seg = kSegments[static_cast<size_t>(bone_index)];
    geom::Vec2<float> parent;
    geom::Vec2<float> child;
    if (!Point2D(frame, seg.parent_landmark, &parent) ||
        !Point2D(frame, seg.child_landmark, &child)) {
        return false;
    }
    *out = child - parent;
    return true;
}

}  // namespace mocap
}  // namespace jpov

#endif  // JPOV_MOCAP_ALGO_MOCAP_LANDMARKS_H_
