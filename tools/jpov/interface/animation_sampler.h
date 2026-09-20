// JPOV Skeleton — 外部动画素材的【时间采样】：给定时刻 → 一个 SkeletonPose（CPU 侧，GL-free）
//
// 定位：interface/animation_clip.h 的 FBXClip 是「一段外部动作」的容器（源骨架 + 帧频 + 原始
// 全帧）；本文件是它的**取帧原语** —— 给定时刻 t（秒），产出该时刻的位姿 SkeletonPose。
// JPOV 本身不做播放/时间轴（见 skeleton_types.h 顶部：一段"动作"只是用户自选的一组 pose 的
// 顺序推进），故"某时刻落在哪一帧、两帧之间怎么过渡"这条规则显式收在此处、由消费方自己按需
// 调用（当前消费方：FBX 观察器逐帧取姿；将来 retarget 取源姿态同款）。
//
// 采样规则（= 一段动画素材被播放时的语义）：
//   1. 帧网格：f = t · fps（0 起）。源文件以 fps 等间隔落 key，loader 已逐帧在帧网格点上取满，
//      故"t 落在哪两帧之间"是纯算术（floor），不涉及 key 时间戳查询。
//      k = floor(f) 为起始帧下标，ratio = f − k ∈ [0,1) 为其间权重。
//   2. 插值（"合理的 pose 差值"）：每骨旋转用**球面线性插值** geom::Slerp —— 单位四元数姿态的
//      标准插值：等角速度、结果仍是单位四元数（若改用矩阵/四元数的线性插值，会引入非旋转的
//      收缩分量，姿态在被插值时"变短"）。根位移 root_offset 是纯平移量，用线性插值即可。
//   3. 循环：时域 = [0, LoopDuration)，时长 = 帧数 / fps（第 i 帧位于 t = i/fps）。t 先按 fmod
//      归一到该区间（负 t 同样归一到正区间）。到末尾后**回绕到首帧**（k+1 取模），于是"末帧 ↔
//      首帧"之间照样插值 —— 循环动作（如舞）反复播放在接缝处不会卡一帧。
//   4. 帧精确：t 落在帧网格上时直接**逐值拷贝**该帧，不引入任何插值浮点误差
//      （“按 fbx 的 fps 播放”就是这种情形）。判据是插值权重 ratio ≈ 0：t = k/fps 经
//      t·fps 的浮点往返会有±1e-16 量级的残差（如 250/30×30 = 250.00000000000003），
//      故阈值内（kFrameEpsilon）一律判为“就在帧上”。
//
// 边界（不 fallback，直接 LOG(FATAL)）：frames 为空 / fps ≤ 0 / 帧 bone_count ≤ 0 /
//   帧内 joint_rotation 尺寸与 bone_count 不符。
//
// 职责边界：本文件只做"容器 → 某一时刻的位姿"的纯计算，不碰 GL、不做重定向、不改容器内容。

#ifndef JPOV_INTERFACE_ANIMATION_SAMPLER_H_
#define JPOV_INTERFACE_ANIMATION_SAMPLER_H_

#include <cmath>
#include <cstddef>

#include <glog/logging.h>
#include "geom/common/quaternion.h"
#include "geom/common/vec.h"

#include "tools/jpov/interface/animation_clip.h"
#include "tools/jpov/interface/skeleton_types.h"

namespace jpov {

// 一段 clip 的循环时长（秒）= 帧数 / 帧频。
// 第 i 帧位于 t = i / fps，故首帧(t=0)到"再回到首帧"的周期 = 帧数 / fps。
// Pre-condition: clip.frames 非空且 clip.frames_per_second > 0（违反 → LOG(FATAL)）。
inline double ClipLoopDurationSeconds(const FBXClip& clip) {
    CHECK(!clip.frames.empty())
        << "ClipLoopDurationSeconds: clip.frames 为空（无帧可采样的容器）";
    CHECK_GT(clip.frames_per_second, 0.0)
        << "ClipLoopDurationSeconds: frames_per_second 必须 > 0，收到 "
        << clip.frames_per_second;
    return static_cast<double>(clip.frames.size()) / clip.frames_per_second;
}

// 帧网格判定阈值（帧为单位）：t 落在帧上时，t·fps 的浮点往返残差量级 ~1e-16，
// 4e-6 帧 ≈ 130ns@30fps —— 远小于任何有意义的插值权重，纯为吸收浮点误差。
inline constexpr float kFrameEpsilon = 4e-6f;

// 取 t 秒处的位姿，写入 out（覆盖写，不追加）。
//
//   clip         : 外部动画素材（帧频 > 0、frames 非空；帧与帧须同构）。
//   time_seconds : 动画时间（秒）。按循环语义归一（见文件头第 3 条），无需调用方预处理。
//   out          : 输出位姿（非空）。bone_count / joint_rotation 尺寸 = 该 clip 的骨数，
//                  joint_rotation 全单位四元数，root_offset 为插值后的根位移。
//                  ⚠️ **单位**：root_offset 是**长度量**，仍为该 clip 的**源单位**（见
//                  FBXClip::unit_meters）。本函数不做单位换算 —— 交给米制下游前请自行
//                  乘 unit_meters（否则位移会被放大 1/unit_meters 倍，见 animation_clip.h 注释）。
//   返回         : 本次采样用的**起始帧下标 k**（= floor(t·fps) 归一后的值）。供调用方显示/
//                  日志用（"当前在第几帧"），采样结果本身与返回值无关。
//
// Pre-condition: out != nullptr；clip 非空且 fps > 0；帧 bone_count > 0 且 joint_rotation
//   尺寸 == bone_count（违反 → LOG(FATAL)）。
inline int SampleClipPose(const FBXClip& clip, double time_seconds,
                          SkeletonPose* out /*output*/) {
    CHECK(out != nullptr) << "SampleClipPose: out 不能为空";

    const double fps = clip.frames_per_second;
    const double duration = ClipLoopDurationSeconds(clip);  // 含非空/fps>0 的 CHECK
    const size_t frame_count = clip.frames.size();

    // 1) 循环归一：t → [0, duration)。
    double t = std::fmod(time_seconds, duration);
    if (t < 0.0) {
        t += duration;  // fmod 对负数是负余数 → 归一到正区间
    }

    // 2) 帧网格位置 → 起始帧 k（防浮点上溢到 frame_count）+ 权重 ratio。
    const double f = t * fps;
    size_t k = static_cast<size_t>(std::floor(f));
    if (k >= frame_count) {
        // 仅当 t 因浮点误差落到 duration 上界时发生（正常归一后 t < duration）。
        k = frame_count - 1;
    }
    const float ratio = static_cast<float>(f - static_cast<double>(k));
    const size_t k_next = (k + 1) % frame_count;  // 回绕：末帧 → 首帧

    const SkeletonPose& a = clip.frames[k];
    const SkeletonPose& b = clip.frames[k_next];
    CHECK_GT(a.bone_count, 0)
        << "SampleClipPose: frame[" << k << "].bone_count 必须 > 0";
    CHECK_EQ(a.joint_rotation.size(), static_cast<size_t>(a.bone_count))
        << "SampleClipPose: frame[" << k << "] joint_rotation 尺寸 "
        << a.joint_rotation.size() << " 应 == bone_count " << a.bone_count;
    CHECK_EQ(b.bone_count, a.bone_count)
        << "SampleClipPose: frame[" << k_next << "].bone_count "
        << b.bone_count << " 与 frame[" << k << "] 不一致（同一段 clip 的帧须同构）";
    CHECK_EQ(b.joint_rotation.size(), static_cast<size_t>(a.bone_count))
        << "SampleClipPose: frame[" << k_next << "] joint_rotation 尺寸与 bone_count 不符";

    const size_t nb = static_cast<size_t>(a.bone_count);
    out->bone_count = a.bone_count;
    out->joint_rotation.resize(nb);

    if (ratio < kFrameEpsilon) {
        // 帧精确路径（见文件头第 4 条），不引入插值误差。
        for (size_t i = 0; i < nb; ++i) {
            out->joint_rotation[i] = a.joint_rotation[i];
        }
        out->root_offset = a.root_offset;
        return static_cast<int>(k);
    }

    for (size_t i = 0; i < nb; ++i) {
        out->joint_rotation[i] =
            geom::Slerp(a.joint_rotation[i], b.joint_rotation[i], ratio);
    }
    // 根位移：纯平移量，线性插值。
    const Vec3f& pa = a.root_offset;
    const Vec3f& pb = b.root_offset;
    out->root_offset = Vec3f(pa.x() + ratio * (pb.x() - pa.x()),
                             pa.y() + ratio * (pb.y() - pa.y()),
                             pa.z() + ratio * (pb.z() - pa.z()));
    return static_cast<int>(k);
}

// 取 t 秒处落在哪一帧、以及帧内小数位置 —— **不做插值**，只做帧网格定位。
//
// 用途：走 **GPU 双帧插值**（SkinnedInstanceState 的 pose_a/pose_b/ratio）时，CPU 侧不能
//   先把姿态插好再上传（那样 atlas 里存的就是"已插值帧"，GPU 再插一次 = 双重插值，
//   运动会被压平/过冲）。正确分工：CPU 只给出「哪两帧 + 权重」，插值交给蒙皮 VS。
//
//   out_frame_index : 起始帧下标 k = floor(t·fps)（已按循环归一 + 防上溢）。
//   out_ratio       : 帧内小数 [0,1)，t 恰落帧网格时为 0。
//   返回            : 帧总数（供调用方回绕 (k+1)）。
//
// Pre-condition: clip 非空且 fps > 0；两个输出指针非空。
inline int LocateClipFrame(const FBXClip& clip, double time_seconds,
                           int* out_frame_index /*output*/,
                           float* out_ratio /*output*/) {
    CHECK(out_frame_index != nullptr && out_ratio != nullptr)
        << "LocateClipFrame: 输出指针不能为空";
    const double fps = clip.frames_per_second;
    const double duration = ClipLoopDurationSeconds(clip);  // 含非空/fps>0 的 CHECK
    const size_t frame_count = clip.frames.size();

    double t = std::fmod(time_seconds, duration);
    if (t < 0.0) {
        t += duration;
    }
    const double f = t * fps;
    size_t k = static_cast<size_t>(std::floor(f));
    double frac = f - static_cast<double>(k);
    if (k >= frame_count) {
        // 仅当 t 因浮点舍入落到 duration 上界时发生（正常归一后 t < duration，
        // 且 duration*fps == frame_count 时 f 仍 < frame_count）。
        k = frame_count - 1;
        // ratio 必须跟着夹到 0：否则 f 略超上界时 frac 会 **> 1**，GPU 侧变成
        //   「外推」而非插值（pose_a/pose_b 权重和 >1，姿态被放大）—— 静默几何错误。
        frac = 0.0;
    }
    *out_frame_index = static_cast<int>(k);
    *out_ratio = static_cast<float>(frac);
    return static_cast<int>(frame_count);
}

}  // namespace jpov

#endif  // JPOV_INTERFACE_ANIMATION_SAMPLER_H_
