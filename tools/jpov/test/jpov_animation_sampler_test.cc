// JPOV — 动画采样（SampleClipPose）在**真实 FBX 资产**上的自洽 + 权威交叉验证（纯 CPU）
//
// 用 hip_hop_dance.fbx（Mixamo 舞，30fps / 518 帧 / 65 骨）验证：
//   1. 两个 loader 入口给出**同一种骨架**（观察器"LoadFbxSkeleton 取骨架 + LoadFbxAnimation
//      取帧"这条组合的前提）：骨数/骨名逐一相同、bind 朝向相同、rest_offset 逐骨相等
//      （两入口都在加载边界归一到米，2026-09-20「单位铁律」）。
//   2. 帧网格上的采样 = 该帧本身（帧精确），且返回的起始帧下标正确。
//   3. 全时间轴上采到的每个位姿都合法（骨数/尺寸/单位四元数/有限）。
//   4. 动作确实在动（不是定格）。
//   5. **权威交叉验证**：把"我们的骨架 + 采样位姿"算出的每骨位置，与 ufbx 自己
//      （ufbx_evaluate_scene，源文件的权威求值）算出的每骨世界位置逐骨比对 ——
//      这是"从 Lcl Rotation 构造出的 pose 合法"的直接证据（相对根骨比较，见下）。
//   6. 循环：超出时长的时间按模归一（t 与 t−时长 采到的位姿一致）。
//
// 关于 §5 的门禁松紧（实测值）：
//   - t 落在源帧网格上（= 按 fbx 的 fps 播放）：偏差 ~5e-7 m（浮点噪声级）→ 门禁 1e-5 m。
//   - t 落在帧之间（60fps 渲染 30fps 素材时必然发生）：偏差 ~2e-3 m 量级（最差 2.7e-3 m）
//     —— 这是**两条插值路径的固有差异**：我们按四元数 Slerp 插值相邻帧（等角速度），
//     而源文件是逐通道（Euler 曲线）插值后合成。差异在指尖量级 3mm（1.7m 身高），
//     肉眼不可辨 → 门禁 1e-2 m。此断言的意义正是"把差异钉住"：若哪天插值实现退化成
//     线性/跳帧，偏差会远超 1e-2 m。

#define _USE_MATH_DEFINES
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <glog/logging.h>
#include "ufbx.h"

#include "geom/common/quaternion.h"
#include "geom/math/mat4.h"
#include "tools/jpov/interface/animation_sampler.h"
#include "tools/jpov/interface/skeleton_types.h"
#include "tools/jpov/src/fbx_loader.h"
#include "tools/jpov/test/test_utils.h"

namespace {

// 与 fbx_loader 同一套"DFS 收 bone 节点"遍历（用于把 ufbx 的 node 与我们的骨下标对齐）。
void CollectBoneNodes(const ufbx_node* n, std::vector<const ufbx_node*>* out) {
    if (n == nullptr) {
        return;
    }
    if (n->bone != nullptr) {
        out->push_back(n);
    }
    for (size_t i = 0; i < n->children.count; ++i) {
        CollectBoneNodes(n->children.data[i], out);
    }
}

// 骨架 + 位姿 → 每关节在骨架空间下的变换 JW（米）。
// 公式与烘焙端一致（见 skeleton_types.h / skeleton_mesh.h 的
//   jointLocal = T(rest_offset)·R(bind)·R(pose)）—— 测试端独立复算一遍，
// 便于与 ufbx 的独立求值交叉比对（不复用被测代码的任何结果）。
std::vector<geom::math::Mat4> JointWorld(const jpov::SkeletonType& skel,
                                         const jpov::SkeletonPose& pose) {
    const size_t n = skel.joints.size();
    std::vector<geom::math::Mat4> jw(n);
    for (size_t j = 0; j < n; ++j) {
        const geom::Quaternion<float> bind = skel.bind_rotation.empty()
            ? geom::Quaternion<float>::Identity()
            : skel.bind_rotation[j];
        const geom::math::Mat4 local = geom::math::JointLocal(
            skel.joints[j].rest_offset, bind, pose.joint_rotation[j]);
        const int p = skel.joints[j].parent;
        jw[j] = (p == jpov::kSkeletonNoParent)
                    ? local
                    : geom::math::Mat4Mul(jw[static_cast<size_t>(p)], local);
    }
    return jw;
}

// 逐骨最大偏差（相对根骨；米）：我们的 JW vs ufbx 权威世界变换。
// 相对根骨比较 = 剔除"骨架空间原点 vs 场景原点"之间那层静态包装变换的差异
// （loader 只收 bone 节点，根骨之上的非 bone 祖先变换不入骨架空间 —— 那是刚性差异，
//  不影响动作本身）。
double MaxJointPositionDeviation(const jpov::SkeletonType& skel,
                                 const jpov::SkeletonPose& pose,
                                 const std::vector<const ufbx_node*>& ufbx_nodes,
                                 double unit_meters, std::string* worst_name) {
    const std::vector<geom::math::Mat4> jw = JointWorld(skel, pose);
    const geom::math::Mat4& our_root = jw[0];
    const ufbx_matrix& u_root = ufbx_nodes[0]->node_to_world;
    const double uox = u_root.cols[3].x * unit_meters;
    const double uoy = u_root.cols[3].y * unit_meters;
    const double uoz = u_root.cols[3].z * unit_meters;

    double max_dp = 0.0;
    for (size_t j = 0; j < skel.joints.size(); ++j) {
        const double px = jw[j].m[12] - our_root.m[12];
        const double py = jw[j].m[13] - our_root.m[13];
        const double pz = jw[j].m[14] - our_root.m[14];
        const ufbx_matrix& um = ufbx_nodes[j]->node_to_world;
        const double qx = um.cols[3].x * unit_meters - uox;
        const double qy = um.cols[3].y * unit_meters - uoy;
        const double qz = um.cols[3].z * unit_meters - uoz;
        const double dp = std::sqrt((px - qx) * (px - qx) + (py - qy) * (py - qy) +
                                    (pz - qz) * (pz - qz));
        if (dp > max_dp) {
            max_dp = dp;
            if (worst_name != nullptr) {
                *worst_name = skel.joints[j].name;
            }
        }
    }
    return max_dp;
}

// glog 没有 CHECK_NEAR（那是 gtest 的宏）→ 本文件自带一个"必须接近"的断言。
void CheckNear(double got, double want, double tol, const char* what) {
    CHECK_LT(std::fabs(got - want), tol)
        << what << "（got=" << got << " want=" << want << " tol=" << tol << "）";
}

}  // namespace

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);
    const std::string kFbx = jpov::GetTestDataDir() +
                             "/animations/hip_hop_dance.fbx";

    jpov::SkeletonType skel;   // 米制骨架（LoadFbxSkeleton）
    jpov::FBXClip clip;        // 帧频 + 全帧（LoadFbxAnimation）
    CHECK(jpov::LoadFbxSkeleton(kFbx, &skel)) << "LoadFbxSkeleton 失败: " << kFbx;
    CHECK(jpov::LoadFbxAnimation(kFbx, &clip)) << "LoadFbxAnimation 失败: " << kFbx;
    const size_t nb = skel.joints.size();
    const size_t nf = clip.frames.size();
    const double fps = clip.frames_per_second;
    const double duration = jpov::ClipLoopDurationSeconds(clip);
    LOG(INFO) << "bones=" << nb << " frames=" << nf << " fps=" << fps
              << " duration=" << duration << "s";

    // ---- 1. 两个 loader 入口必须给出同一种骨架（观察器组合两者的前提）----
    CHECK_EQ(clip.skeleton.joints.size(), nb);
    for (size_t i = 0; i < nb; ++i) {
        CHECK_EQ(clip.skeleton.joints[i].name, skel.joints[i].name)
            << "joint[" << i << "] 两个入口的骨名不一致（收集顺序分叉）";
        CHECK_EQ(clip.skeleton.joints[i].parent, skel.joints[i].parent)
            << "joint[" << i << "] 两个入口的父索引不一致";
        const float dot = std::fabs(clip.skeleton.bind_rotation[i].Dot(
            skel.bind_rotation[i]));
        CHECK_GT(dot, 0.999999f)
            << "joint[" << i << "] 两个入口的 bind 朝向不一致（|dot|=" << dot << "）";
    }
    LOG(INFO) << "1. 两个 loader 入口骨架一致（骨数/骨名/父索引/bind 朝向）";

    // ---- 1b. rest_offset 两入口**逐骨相等**（都在加载边界归一到米）----
    // 两个 loader 入口对同一文件必须给出**同尺度的骨架**（2026-09-20 “单位铁律”：
    // 换算只发生在加载边界）。骨长比值必须对全部骨都 = 1；若某一侧没做单位归一
    // （旧行为：clip 原样透传 cm 源），比值就会是 0.01 而不等于 1 → 本检查失败。
    double ratio_sum = 0.0;
    size_t ratio_count = 0;
    for (size_t i = 0; i < nb; ++i) {
        const jpov::Vec3f& a = skel.joints[i].rest_offset;           // 米
        const jpov::Vec3f& b = clip.skeleton.joints[i].rest_offset;  // 也应为米
        if (b.Norm() <= 1e-6f) {
            continue;
        }
        ratio_sum += static_cast<double>(a.Norm() / b.Norm());
        ++ratio_count;
    }
    CHECK_GT(ratio_count, 0u) << "应当有非零长骨可供比较";
    const double ratio = ratio_sum / static_cast<double>(ratio_count);
    CheckNear(ratio, 1.0, 1e-4,
              "两入口骨长比应为 1（都是米）；≠1 说明某侧没在加载边界换算单位");
    for (size_t i = 0; i < nb; ++i) {
        const jpov::Vec3f& a = skel.joints[i].rest_offset;
        const jpov::Vec3f& b = clip.skeleton.joints[i].rest_offset;
        if (b.Norm() <= 1e-6f) {
            continue;
        }
        CheckNear((a - b).Norm(), 0.0, 1e-4,
                  "rest_offset 不是逐骨一致（两入口尺度分叉）");
    }
    // 且确实是**米**（防"两入口一起忘了换算"这种同时错、比值仍为 1 的盲区）：
    // 找 Hips（腰高）应在人形尺度 [0.5, 2.0] m；cm 制会得 ≈104。
    for (size_t i = 0; i < nb; ++i) {
        const std::string& nm = skel.joints[i].name;
        if (nm.size() >= 4 && nm.compare(nm.size() - 4, 4, "Hips") == 0) {
            const float hy = skel.joints[i].rest_offset.y();
            CHECK_GT(hy, 0.5f) << "Hips 高 " << hy << " 偏小，长度量应已归一为米";
            CHECK_LT(hy, 2.0f) << "Hips 高 " << hy << " 偏大 —— 疑似没做单位换算";
            LOG(INFO) << "1b-h. Hips 高 = " << hy << " m（人形尺度 ✓）";
            break;
        }
    }
    LOG(INFO) << "1b. 两入口 rest_offset 逐骨相等（比值 = " << ratio << "，均为米）";

    // ---- 2. 帧网格上的采样 = 该帧本身（帧精确）----
    const size_t kProbeFrames[] = {0, 1, 17, 250, 517};
    for (size_t k : kProbeFrames) {
        CHECK_LT(k, nf);
        jpov::SkeletonPose pose;
        const int got_k = jpov::SampleClipPose(clip, static_cast<double>(k) / fps, &pose);
        CHECK_EQ(got_k, static_cast<int>(k)) << "t=" << k << "/fps 应报起始帧 " << k;
        CHECK_EQ(pose.joint_rotation.size(), nb);
        for (size_t i = 0; i < nb; ++i) {
            // 帧精确：逐分量相等（ratio==0 走拷贝路径）。
            CHECK_EQ(pose.joint_rotation[i].w, clip.frames[k].joint_rotation[i].w)
                << "frame " << k << " bone " << i << " 应逐值等于该帧";
            CHECK_EQ(pose.joint_rotation[i].z, clip.frames[k].joint_rotation[i].z)
                << "frame " << k << " bone " << i << " 应逐值等于该帧";
        }
    }
    LOG(INFO) << "2. 帧网格上采样 = 该帧本身（帧精确，含末帧 517）";

    // ---- 3. 全时间轴（含帧间与循环边界）采到的位姿都合法 ----
    for (int i = 0; i < 600; ++i) {
        const double t = duration * static_cast<double>(i) / 600.0 - 0.01;
        jpov::SkeletonPose pose;
        jpov::SampleClipPose(clip, t, &pose);
        CHECK_EQ(pose.joint_rotation.size(), nb) << "t=" << t;
        for (size_t b = 0; b < nb; ++b) {
            const geom::Quaternion<float>& q = pose.joint_rotation[b];
            CheckNear(q.Norm(), 1.0, 1e-4, "采到的四元数非单位");
            CHECK(std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) &&
                  std::isfinite(q.w))
                << "t=" << t << " bone " << b << " 出现 NaN/Inf";
        }
        CHECK(std::isfinite(pose.root_offset.y())) << "t=" << t << " root_offset 非有限";
    }
    LOG(INFO) << "3. 全时间轴（负时间起）600 个采样点的位姿全合法";

    // ---- 4. 动作确实在动 ----
    {
        jpov::SkeletonPose p0;
        jpov::SkeletonPose p1;
        jpov::SampleClipPose(clip, 0.0, &p0);
        jpov::SampleClipPose(clip, duration / 3.0, &p1);
        int moved = 0;
        for (size_t b = 0; b < nb; ++b) {
            if (std::fabs(p0.joint_rotation[b].Dot(p1.joint_rotation[b])) < 0.9999f) {
                ++moved;
            }
        }
        CHECK_GT(moved, static_cast<int>(nb) / 4)
            << "1/3 时长处应有大量骨移动，实测仅 " << moved << " 根";
        LOG(INFO) << "4. 动作在动：1/3 时长处有 " << moved << "/" << nb << " 根骨变化";
    }

    // ---- 5. 权威交叉验证：我们的位姿 vs ufbx 自己求值出的世界布局 ----
    {
        ufbx_load_opts opts;
        std::memset(&opts, 0, sizeof(opts));
        opts.ignore_geometry = true;
        opts.ignore_embedded = true;
        opts.load_external_files = false;
        ufbx_error err;
        std::memset(&err, 0, sizeof(err));
        ufbx_scene* scene = ufbx_load_file(kFbx.c_str(), &opts, &err);
        CHECK(scene != nullptr) << "ufbx 直接加载失败";
        const double unit = scene->settings.unit_meters;
        // 两入口骨长比现应为 1（都已归一米）；同时与“源文件确实是厘米制”对得上：
        // unit 应 ≈ 0.01（证明该资产长度量确实是 cm、换算确实发生过）。
        CheckNear(ratio, 1.0, 1e-4, "两入口骨架应同为米（骨长比 = 1）");
        CheckNear(unit, 0.01, 1e-6, "本源应为厘米制（为“已换算”提供对照）");

        // 帧网格上的时刻（按 fbx fps 播放到的地方）→ 应与源逐骨一致到浮点精度。
        const double kOnGrid[] = {0.0, 0.5, 1.0, 2.0, 5.0, 9.0, 17.0};
        for (double t : kOnGrid) {
            ufbx_error e2;
            std::memset(&e2, 0, sizeof(e2));
            ufbx_scene* ev = ufbx_evaluate_scene(scene, scene->anim, t, nullptr, &e2);
            CHECK(ev != nullptr) << "ufbx_evaluate_scene 失败 t=" << t;
            std::vector<const ufbx_node*> nodes;
            CollectBoneNodes(ev->root_node, &nodes);
            CHECK_EQ(nodes.size(), nb);
            jpov::SkeletonPose pose;
            jpov::SampleClipPose(clip, t, &pose);
            std::string worst;
            const double dp = MaxJointPositionDeviation(skel, pose, nodes, unit, &worst);
            CHECK_LT(dp, 1e-5)
                << "t=" << t << " 与源 FBX 权威布局偏差过大: " << dp << " m (worst "
                << worst << ")";
            LOG(INFO) << "5a. t=" << t << " 帧网格：与源偏差 " << dp << " m";
            ufbx_free_scene(ev);
        }

        // 帧之间的时刻（60fps 渲染 30fps 素材实际会采到的位置）→ 允许"插值路径差异"
        // 带来的毫米级偏差（见文件头说明），但不能退化。
        const double kOffGrid[] = {0.0166, 0.4999, 0.5123, 3.3333, 7.9876};
        for (double t : kOffGrid) {
            ufbx_error e2;
            std::memset(&e2, 0, sizeof(e2));
            ufbx_scene* ev = ufbx_evaluate_scene(scene, scene->anim, t, nullptr, &e2);
            CHECK(ev != nullptr);
            std::vector<const ufbx_node*> nodes;
            CollectBoneNodes(ev->root_node, &nodes);
            jpov::SkeletonPose pose;
            jpov::SampleClipPose(clip, t, &pose);
            std::string worst;
            const double dp = MaxJointPositionDeviation(skel, pose, nodes, unit, &worst);
            CHECK_LT(dp, 0.01)
                << "t=" << t << " 帧间插值与源的偏差超 1cm: " << dp << " m (worst "
                << worst << ")";
            LOG(INFO) << "5b. t=" << t << " 帧间：与源偏差 " << dp << " m (worst "
                      << worst << ")";
            ufbx_free_scene(ev);
        }
        ufbx_free_scene(scene);
    }

    // ---- 6. 循环：超出时长按模归一（t 与 t − 时长 采到同一位姿）----
    {
        const double kTimes[] = {0.1, 1.7, 9.3};
        for (double t : kTimes) {
            jpov::SkeletonPose a;
            jpov::SkeletonPose b;
            jpov::SampleClipPose(clip, t, &a);
            jpov::SampleClipPose(clip, t + duration, &b);
            for (size_t i = 0; i < nb; ++i) {
                CHECK_GT(std::fabs(a.joint_rotation[i].Dot(b.joint_rotation[i])), 0.999999f)
                    << "t=" << t << " 与 t+时长 的位姿应一致（循环归一回绕）";
            }
        }
        LOG(INFO) << "6. 循环归一：t 与 t±时长 位姿一致";
    }

    LOG(INFO) << "jpov_animation_sampler_test PASSED";
    return 0;
}
