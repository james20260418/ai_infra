// JPOV Mocap — 方向场算法实现。
//
// 这里只做「每帧每骨世界方向」的估计。**不做**填帧、不做 yaw、不做 root —— 那些是
// mocap_fit.cc 的职责，分开才能让「算法差异」和「拟合差异」在批量报告里互不污染。

#include "tools/jpov/mocap/algo/mocap_algorithms.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include <glog/logging.h>
#include "geom/common/vec.h"
#include "tools/jpov/mocap/algo/mocap_landmarks.h"
#include "tools/jpov/mocap/algo/mocap_math.h"

namespace jpov {
namespace mocap {
namespace {

bool IsHandOrToe(const char* name) {
    const std::string s = name;
    return s.find("Hand") != std::string::npos || s.find("ToeBase") != std::string::npos;
}

// 把缺帧的 2D 点按最近可用帧补齐（仅用于平滑/算法内部；DirectionSequence 仍用
// 原始帧的 valid 标记，不把补出来的方向伪装成观测）。
TrackFrame FillNearestFrame(const MocapTrack& track, size_t index) {
    if (track.frames[index].has_landmarks) {
        return track.frames[index];
    }
    size_t best = index;
    int best_dist = std::numeric_limits<int>::max();
    for (size_t i = 0; i < track.frames.size(); ++i) {
        if (!track.frames[i].has_landmarks) {
            continue;
        }
        const int dist = std::abs(static_cast<int>(i) - static_cast<int>(index));
        if (dist < best_dist) {
            best = i;
            best_dist = dist;
        }
    }
    TrackFrame out = track.frames[best];
    out.t_seconds = track.frames[index].t_seconds;
    return out;
}

// 对 2D 关键点做逐点零相位平滑（每个 landmark 的 x / y 各自一条序列）。
std::vector<TrackFrame> SmoothTrack2D(const MocapTrack& track, int window) {
    if (track.frames.size() < 3 || window <= 1) {
        return track.frames;
    }
    std::vector<TrackFrame> filled;
    filled.reserve(track.frames.size());
    for (size_t i = 0; i < track.frames.size(); ++i) {
        filled.push_back(FillNearestFrame(track, i));
    }
    std::vector<float> xs(track.frames.size(), 0.0f);
    std::vector<float> ys(track.frames.size(), 0.0f);
    std::vector<TrackFrame> out = filled;
    for (int lm = 0; lm < kLandmarkCount; ++lm) {
        for (size_t i = 0; i < filled.size(); ++i) {
            xs[i] = filled[i].landmarks[static_cast<size_t>(lm)].x_px;
            ys[i] = filled[i].landmarks[static_cast<size_t>(lm)].y_px;
        }
        const std::vector<float> sx = internal::SmoothZeroPhase(xs, window);
        const std::vector<float> sy = internal::SmoothZeroPhase(ys, window);
        for (size_t i = 0; i < filled.size(); ++i) {
            out[i].landmarks[static_cast<size_t>(lm)].x_px = sx[i];
            out[i].landmarks[static_cast<size_t>(lm)].y_px = sy[i];
        }
    }
    for (size_t i = 0; i < out.size(); ++i) {
        out[i].has_landmarks = track.frames[i].has_landmarks;
    }
    return out;
}

// 对 world z 做时间中值滤波（压住深度符号翻转；x/y 保持不动）。
std::vector<TrackFrame> MedianWorldZ(const MocapTrack& track, int window) {
    std::vector<TrackFrame> filled;
    filled.reserve(track.frames.size());
    for (size_t i = 0; i < track.frames.size(); ++i) {
        filled.push_back(FillNearestFrame(track, i));
    }
    std::vector<TrackFrame> out = filled;
    std::vector<float> zs(track.frames.size(), 0.0f);
    for (int lm = 0; lm < kLandmarkCount; ++lm) {
        for (size_t i = 0; i < filled.size(); ++i) {
            zs[i] = filled[i].world[static_cast<size_t>(lm)].z();
        }
        const std::vector<float> smoothed = internal::MedianFilter(zs, window);
        for (size_t i = 0; i < filled.size(); ++i) {
            out[i].world[static_cast<size_t>(lm)].m[2] = smoothed[i];
        }
    }
    for (size_t i = 0; i < out.size(); ++i) {
        out[i].has_world = track.frames[i].has_world;
    }
    return out;
}

DirectionSequence BuildWorldZ(const MocapTrack& track, const SkeletonType& skeleton,
                             bool smooth) {
    DirectionSequence seq;
    seq.algorithm_name = smooth ? "worldz_t" : "worldz";
    seq.px_per_meter = EstimatePixelsPerMeter(track);
    seq.bone_length_m = EstimateBoneLengths(track, skeleton);
    seq.frames.resize(track.frames.size());
    for (size_t i = 0; i < track.frames.size(); ++i) {
        for (int j = 1; j < kMixamo23BoneCount; ++j) {
            Vec3f v;
            if (!RawBoneVector3D(track.frames[i], j, &v)) {
                continue;
            }
            if (!internal::NormalizeVec3(&v)) {
                continue;
            }
            seq.frames[i].direction[static_cast<size_t>(j)] = v;
            seq.frames[i].valid[static_cast<size_t>(j)] = true;
        }
    }
    if (smooth && !seq.frames.empty()) {
        // 逐骨对方向分量做零相位平滑，再归一；缺帧的骨沿用最近有效值（平滑需要连续序列）。
        for (int j = 1; j < kMixamo23BoneCount; ++j) {
            std::vector<float> xs(seq.frames.size(), 0.0f);
            std::vector<float> ys(seq.frames.size(), 0.0f);
            std::vector<float> zs(seq.frames.size(), 0.0f);
            std::vector<bool> valid(seq.frames.size(), false);
            for (size_t i = 0; i < seq.frames.size(); ++i) {
                const bool ok = seq.frames[i].valid[static_cast<size_t>(j)];
                valid[i] = ok;
                const Vec3f v = seq.frames[i].direction[static_cast<size_t>(j)];
                xs[i] = v.x();
                ys[i] = v.y();
                zs[i] = v.z();
            }
            auto fill_forward = [&](std::vector<float>* values) {
                float last = 0.0f;
                bool has = false;
                for (size_t i = 0; i < values->size(); ++i) {
                    if (valid[i]) {
                        last = (*values)[i];
                        has = true;
                    } else if (has) {
                        (*values)[i] = last;
                    }
                }
                // 序列开头没有前值 -> 用后一个有效值回填。
                float next = 0.0f;
                bool has_next = false;
                for (size_t r = values->size(); r > 0; --r) {
                    const size_t i = r - 1;
                    if (valid[i]) {
                        next = (*values)[i];
                        has_next = true;
                    } else if (has_next) {
                        (*values)[i] = next;
                    }
                }
            };
            fill_forward(&xs);
            fill_forward(&ys);
            fill_forward(&zs);
            const std::vector<float> sx = internal::SmoothZeroPhase(xs, 5);
            const std::vector<float> sy = internal::SmoothZeroPhase(ys, 5);
            const std::vector<float> sz = internal::SmoothZeroPhase(zs, 5);
            for (size_t i = 0; i < seq.frames.size(); ++i) {
                Vec3f v{sx[i], sy[i], sz[i]};
                if (internal::NormalizeVec3(&v)) {
                    seq.frames[i].direction[static_cast<size_t>(j)] = v;
                    // 平滑把相邻帧信息带进来：只保留原本有效的观测，避免把补帧当证据。
                    seq.frames[i].valid[static_cast<size_t>(j)] = valid[i];
                }
            }
        }
    }
    return seq;
}

DirectionSequence BuildLift2D(const MocapTrack& track, bool temporal) {
    const std::array<float, kMixamo23BoneCount> lengths =
        EstimateBoneLengths(track, Mixamo23Skeleton(1.6f));
    const float px_per_meter = EstimatePixelsPerMeter(track);
    const float scale = std::max(px_per_meter, 1.0f);

    std::vector<TrackFrame> frames2d;
    std::vector<TrackFrame> frames3d;
    if (temporal) {
        frames2d = SmoothTrack2D(track, 5);
        frames3d = MedianWorldZ(track, 5);
    } else {
        frames2d = track.frames;
        frames3d = track.frames;
    }

    DirectionSequence seq;
    seq.algorithm_name = temporal ? "lift2d_t" : "lift2d";
    seq.px_per_meter = px_per_meter;
    seq.bone_length_m = lengths;
    seq.frames.resize(track.frames.size());

    // 深度符号的跨帧记忆（单目深度歧义只能借 world-z；world-z 太小就沿用上帧）。
    std::array<float, kMixamo23BoneCount> prev_sign{};
    std::array<std::vector<float>, kMixamo23BoneCount> depth_series;

    for (size_t i = 0; i < track.frames.size(); ++i) {
        for (int j = 1; j < kMixamo23BoneCount; ++j) {
            const BoneSegment& seg = kSegments[static_cast<size_t>(j)];
            geom::Vec2<float> d2;
            if (!RawBoneVector2D(frames2d[i], j, &d2)) {
                continue;
            }
            const float u = d2.Norm() / scale;
            const float bone_len = lengths[static_cast<size_t>(j)];
            float depth = 0.0f;
            float sign = 0.0f;
            if (bone_len > internal::kMocapEps && u < bone_len * 0.995f) {
                const float mag = std::sqrt(std::max(0.0f, bone_len * bone_len - u * u));
                float dz = 0.0f;
                if (frames3d[i].has_world) {
                    Vec3f a;
                    Vec3f b;
                    if (Point3D(frames3d[i], seg.parent_landmark, &a) &&
                        Point3D(frames3d[i], seg.child_landmark, &b)) {
                        dz = b.z() - a.z();
                    }
                }
                sign = dz >= 0.0f ? 1.0f : -1.0f;
                if (temporal && prev_sign[static_cast<size_t>(j)] != 0.0f &&
                    std::abs(dz) < 0.02f) {
                    sign = prev_sign[static_cast<size_t>(j)];
                }
                depth = sign * mag;
            }
            prev_sign[static_cast<size_t>(j)] = sign;
            Vec3f v{d2.x() / scale, d2.y() / scale, depth};
            if (!internal::NormalizeVec3(&v)) {
                continue;
            }
            seq.frames[i].direction[static_cast<size_t>(j)] = v;
            seq.frames[i].valid[static_cast<size_t>(j)] = true;
            // 记录**平滑前的原始 depth**；后面 SmoothZeroPhase 后再重新归一。
            depth_series[static_cast<size_t>(j)].push_back(depth);
        }
    }

    if (temporal) {
        // 深度分量再做一次零相位平滑（与 Python lift2d_t 同义），再按原方向归一。
        std::array<std::vector<float>, kMixamo23BoneCount> smoothed_depth;
        for (int j = 1; j < kMixamo23BoneCount; ++j) {
            smoothed_depth[static_cast<size_t>(j)] =
                internal::SmoothZeroPhase(depth_series[static_cast<size_t>(j)], 9);
        }
        std::array<size_t, kMixamo23BoneCount> cursor{};
        for (size_t i = 0; i < seq.frames.size(); ++i) {
            for (int j = 1; j < kMixamo23BoneCount; ++j) {
                if (!seq.frames[i].valid[static_cast<size_t>(j)]) {
                    continue;
                }
                Vec3f v = seq.frames[i].direction[static_cast<size_t>(j)];
                const auto& series = smoothed_depth[static_cast<size_t>(j)];
                if (cursor[static_cast<size_t>(j)] < series.size()) {
                    v.m[2] = series[cursor[static_cast<size_t>(j)]++];
                }
                if (internal::NormalizeVec3(&v)) {
                    seq.frames[i].direction[static_cast<size_t>(j)] = v;
                }
            }
        }
    }
    return seq;
}

}  // namespace

std::array<float, kMixamo23BoneCount> EstimateBoneLengths(
    const MocapTrack& track, const SkeletonType& skeleton) {
    CHECK_EQ(skeleton.bone_count(), kMixamo23BoneCount)
        << "EstimateBoneLengths: 需要 Mixamo23 骨架（23 骨），收到 "
        << skeleton.bone_count();
    std::array<std::vector<float>, kMixamo23BoneCount> samples;
    for (const TrackFrame& frame : track.frames) {
        if (!frame.has_world) {
            continue;
        }
        for (int j = 1; j < kMixamo23BoneCount; ++j) {
            Vec3f v;
            if (RawBoneVector3D(frame, j, &v)) {
                const float len = v.Norm();
                if (len > internal::kMocapEps) {
                    samples[static_cast<size_t>(j)].push_back(len);
                }
            }
        }
    }
    std::array<float, kMixamo23BoneCount> out{};
    for (int j = 1; j < kMixamo23BoneCount; ++j) {
        float len = internal::Median(samples[static_cast<size_t>(j)]);
        if (!(len > internal::kMocapEps)) {
            // 没有 world 的骨（或整段没 world）用骨架 rest_offset 长度兜底。
            len = skeleton.joints[static_cast<size_t>(j)].rest_offset.Norm();
        }
        out[static_cast<size_t>(j)] = len;
    }
    return out;
}

float EstimatePixelsPerMeter(const MocapTrack& track) {
    std::vector<float> ratios;
    for (const TrackFrame& frame : track.frames) {
        if (!frame.has_landmarks || !frame.has_world) {
            continue;
        }
        float px_sum = 0.0f;
        float m_sum = 0.0f;
        for (int j = 1; j < kMixamo23BoneCount; ++j) {
            if (IsHandOrToe(kSegments[static_cast<size_t>(j)].joint_name)) {
                continue;
            }
            geom::Vec2<float> d2;
            Vec3f d3;
            if (!RawBoneVector2D(frame, j, &d2) || !RawBoneVector3D(frame, j, &d3)) {
                continue;
            }
            const float px = d2.Norm();
            const float meters = d3.Norm();
            if (px > 4.0f && meters > 1e-3f) {
                px_sum += px;
                m_sum += meters;
            }
        }
        if (m_sum > 1e-6f) {
            ratios.push_back(px_sum / m_sum);
        }
    }
    const float ratio = internal::Median(std::move(ratios));
    return ratio > 1.0f ? ratio : 500.0f;
}

const std::vector<DirectionAlgorithm>& ListDirectionAlgorithms() {
    static const std::vector<DirectionAlgorithm> kAlgorithms = {
        {"worldz",
         "MediaPipe world landmarks 直接求骨方向（深度噪声大，作为基准）",
         [](const MocapTrack& track, const SkeletonType& skeleton) {
             return BuildWorldZ(track, skeleton, /*smooth=*/false);
         }},
        {"worldz_t",
         "worldz + 零相位时间平滑（压深度噪声的时序基线）",
         [](const MocapTrack& track, const SkeletonType& skeleton) {
             return BuildWorldZ(track, skeleton, /*smooth=*/true);
         }},
        {"lift2d",
         "2D 关键点 + 骨长先验反解深度（画面内方向精确）",
         [](const MocapTrack& track, const SkeletonType& skeleton) {
             (void)skeleton;
             return BuildLift2D(track, /*temporal=*/false);
         }},
        {"lift2d_t",
         "lift2d + 零相位 2D/深度平滑 + 深度符号中值保持（抖动最低）",
         [](const MocapTrack& track, const SkeletonType& skeleton) {
             (void)skeleton;
             return BuildLift2D(track, /*temporal=*/true);
         }},
    };
    return kAlgorithms;
}

const DirectionAlgorithm* FindDirectionAlgorithm(const std::string& name) {
    for (const DirectionAlgorithm& algo : ListDirectionAlgorithms()) {
        if (algo.name == name) {
            return &algo;
        }
    }
    return nullptr;
}

}  // namespace mocap
}  // namespace jpov
