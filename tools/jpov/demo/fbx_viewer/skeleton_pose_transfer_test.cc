// JPOV FBX 观察器 — 目标骨架 pose 直搬（无重定向对照组）单测（纯 CPU / GL-free）
//
// 覆盖 skeleton_pose_transfer.h 的两件事：
//   A. 搬运语义：按骨名匹配（顺序/骨数都不同）→ 命中骨**数值原样**写入、未命中骨保持
//      identity、源空 pose = 全 identity。纯合成骨架，可精确断言。
//   B. ⚠️ **实验结论的可执行化**：拿真实资产（hip_hop_dance.fbx 的 pose + mixamo_male.glb
//      的 rest 骨架）跑一次直搬，断言"不做重定向"时**骨段朝向与源差得很远** ——
//      这就是 Danis 要验证的第 1 点（"不加重定向直接上 lcl rotation 会不对"）。
//      本用例**刻意断言"它不对"**：若将来有人把重定向塞进本函数，这里会失败并提醒
//      改错地方（正式重定向属于 M4 的另一个函数）。
//
// 断言纪律（skills/zero-run-code-reading-check）：A 组每条都有能令其失败的实现改动
// （如"顺手"加上 bind 共轭 → A 的数值原样断言失败）。

#define _USE_MATH_DEFINES
#include <cmath>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "geom/common/quaternion.h"
#include "tools/jpov/demo/fbx_viewer/skeleton_pose_transfer.h"
#include "tools/jpov/interface/skeleton_types.h"
#include "tools/jpov/src/fbx_loader.h"
#include "tools/jpov/src/gltf_loader.h"
#include "tools/jpov/test/test_utils.h"

namespace {

constexpr float kPi = 3.14159265358979323846f;

geom::Quaternion<float> RotAxis(float ax, float ay, float az, float deg) {
    const geom::Vec3<float> axis(ax, ay, az);
    return geom::Quaternion<float>::FromAxisAngle(axis, deg * kPi / 180.0f);
}

// 骨架工厂：骨名为 name0..nameN，直链（0 为根）。
jpov::SkeletonType MakeChain(const std::vector<std::string>& names) {
    jpov::SkeletonType s;
    for (size_t i = 0; i < names.size(); ++i) {
        jpov::SkeletonJoint j;
        j.parent = (i == 0) ? jpov::kSkeletonNoParent : static_cast<int>(i - 1);
        j.rest_offset = jpov::Vec3f(0.0f, 1.0f, 0.0f);
        j.name = names[i];
        s.joints.push_back(j);
    }
    return s;
}

jpov::SkeletonPose MakePose(const std::vector<geom::Quaternion<float>>& rots) {
    jpov::SkeletonPose p;
    p.bone_count = static_cast<int>(rots.size());
    p.joint_rotation = rots;
    return p;
}

int IndexOf(const jpov::SkeletonType& s, const std::string& name) {
    for (size_t i = 0; i < s.joints.size(); ++i) {
        if (s.joints[i].name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// 骨架 + 位姿 → 每关节世界朝向（旋转部分，沿树累积）：W[j] = W[parent]·R(bind_j)·R(pose_j)
std::vector<geom::Quaternion<float>> WorldRot(const jpov::SkeletonType& s,
                                              const jpov::SkeletonPose& p) {
    std::vector<geom::Quaternion<float>> w(s.joints.size());
    for (size_t j = 0; j < s.joints.size(); ++j) {
        const geom::Quaternion<float> bind = s.bind_rotation.empty()
            ? geom::Quaternion<float>::Identity() : s.bind_rotation[j];
        const geom::Quaternion<float> pose = p.joint_rotation.empty()
            ? geom::Quaternion<float>::Identity() : p.joint_rotation[j];
        const geom::Quaternion<float> local = bind * pose;
        const int par = s.joints[j].parent;
        w[j] = (par == jpov::kSkeletonNoParent)
                   ? local
                   : w[static_cast<size_t>(par)] * local;
    }
    return w;
}

// 骨段（父→子）的世界方向（单位向量）：dir_j = normalize(W[parent] · rest_offset_j)
geom::Vec3<float> BoneDirection(const jpov::SkeletonType& s,
                                const std::vector<geom::Quaternion<float>>& w,
                                size_t j) {
    const int par = s.joints[j].parent;
    const geom::Vec3<float> off = s.joints[j].rest_offset;
    if (par == jpov::kSkeletonNoParent) {
        return off.Unit();
    }
    return geom::RotateVector(w[static_cast<size_t>(par)], off.Unit());
}

float AngleBetweenDeg(const geom::Vec3<float>& a, const geom::Vec3<float>& b) {
    const float d = std::max(-1.0f, std::min(1.0f, a.Dot(b)));
    return std::acos(d) * 180.0f / kPi;
}

// ── A1. 按骨名匹配（源/目标骨数、顺序都不同）──
void TestMappingByName() {
    // 源：Hips, Spine, LeftArm, LeftHand（4 骨）
    const jpov::SkeletonType src = MakeChain({"Hips", "Spine", "LeftArm", "LeftHand"});
    // 目标：LeftArm, Hips, Unknown（3 骨，顺序不同 + 含一个源里没有的骨）
    const jpov::SkeletonType dst = MakeChain({"LeftArm", "Hips", "Unknown"});

    const geom::Quaternion<float> q_hips = RotAxis(1, 0, 0, 30);
    const geom::Quaternion<float> q_arm = RotAxis(0, 1, 0, 70);
    const jpov::SkeletonPose src_pose =
        MakePose({q_hips, RotAxis(0, 0, 1, 10), q_arm, RotAxis(1, 0, 0, 5)});

    jpov::SkeletonPose dst_pose;
    const jpov_fbx_viewer::PoseTransferStats st =
        jpov_fbx_viewer::TransferPoseByNameNoRetarget(src, src_pose, dst, &dst_pose);

    CHECK_EQ(st.target_bones, 3);
    CHECK_EQ(st.mapped, 2) << "应命中 Hips + LeftArm（Spine/LeftHand 目标里没有，Unknown 源里没有）";
    CHECK_EQ(dst_pose.bone_count, 3);
    CHECK_EQ(dst_pose.joint_rotation.size(), 3u);
    // 命中骨：**数值原样**（逐分量相等 —— 本函数刻意不做任何重定向/共轭）。
    const int i_arm = IndexOf(dst, "LeftArm");
    const int i_hips = IndexOf(dst, "Hips");
    const geom::Quaternion<float>& got_arm = dst_pose.joint_rotation[i_arm];
    CHECK_EQ(got_arm.x, q_arm.x);
    CHECK_EQ(got_arm.y, q_arm.y);
    CHECK_EQ(got_arm.z, q_arm.z);
    CHECK_EQ(got_arm.w, q_arm.w);
    CHECK_EQ(dst_pose.joint_rotation[i_hips].x, q_hips.x);
    CHECK_EQ(dst_pose.joint_rotation[i_hips].w, q_hips.w);
    // 未命中骨：identity（|w|=1，x/y/z=0）。
    const geom::Quaternion<float>& got_unknown =
        dst_pose.joint_rotation[IndexOf(dst, "Unknown")];
    CHECK_EQ(got_unknown.x, 0.0f);
    CHECK_EQ(got_unknown.y, 0.0f);
    CHECK_EQ(got_unknown.z, 0.0f);
    CHECK_EQ(got_unknown.w, 1.0f);
    // root_offset 不搬 → 恒 0。
    CHECK_EQ(dst_pose.root_offset.x(), 0.0f);
    LOG(INFO) << "OK TestMappingByName";
}

// ── A2. 源 pose 为空 = 视为全 identity（走 rest）；骨名命中数不受源 pose 内容影响 ──
void TestEmptySourcePoseIsRest() {
    const jpov::SkeletonType src = MakeChain({"Hips", "LeftArm"});
    const jpov::SkeletonType dst = MakeChain({"LeftArm", "Hips"});
    jpov::SkeletonPose dst_pose;
    const jpov_fbx_viewer::PoseTransferStats st =
        jpov_fbx_viewer::TransferPoseByNameNoRetarget(src, jpov::SkeletonPose{},
                                                      dst, &dst_pose);
    CHECK_EQ(st.mapped, 2) << "骨架里两根同名骨都应计为命中（命中数与源 pose 内容无关）";
    for (const geom::Quaternion<float>& q : dst_pose.joint_rotation) {
        CHECK_EQ(q.w, 1.0f) << "源没有旋转可搬 → 目标全部 identity";
        CHECK_LT(std::fabs(q.x) + std::fabs(q.y) + std::fabs(q.z), 1e-6f);
    }
    LOG(INFO) << "OK TestEmptySourcePoseIsRest";
}

// ── B. 真实资产：直搬后骨段朝向与源差得很远（= 不做重定向确实"不对"）──
void TestNaiveTransferIsWrongOnRealAssets() {
    const std::string fbx = jpov::GetTestDataDir() + "/animations/hip_hop_dance.fbx";
    const std::string glb =
        jpov::GetTestDataDir() + "/object3d/mixamo_male/mixamo_male.glb";

    jpov::SkeletonType src;      // fbx 源骨架（红）
    jpov::FBXClip clip;
    CHECK(jpov::LoadFbxSkeleton(fbx, &src)) << "LoadFbxSkeleton 失败";
    CHECK(jpov::LoadFbxAnimation(fbx, &clip)) << "LoadFbxAnimation 失败";
    std::vector<jpov::SkeletonType> skins;
    CHECK(jpov::LoadGltfSkeleton(glb, &skins)) << "LoadGltfSkeleton 失败";
    CHECK(!skins.empty());
    const jpov::SkeletonType dst = skins[0];   // glb 目标骨架（蓝）

    jpov::SkeletonPose dst_pose;
    const jpov_fbx_viewer::PoseTransferStats st =
        jpov_fbx_viewer::TransferPoseByNameNoRetarget(src, clip.frames[0], dst,
                                                      &dst_pose);
    CHECK_GT(st.mapped, 15) << "两个资产应有大量同名骨（实测 22/23）";

    // 两侧各自的世界朝向 / 骨段方向。
    const std::vector<geom::Quaternion<float>> w_src = WorldRot(src, clip.frames[0]);
    const std::vector<geom::Quaternion<float>> w_dst = WorldRot(dst, dst_pose);

    // 逐骨比"骨段方向"（父→子，肉眼看到的那根杆的方向）：应差很远。
    float max_dir = 0.0f;
    std::string worst;
    int over30 = 0;
    for (size_t i = 0; i < dst.joints.size(); ++i) {
        const int si = IndexOf(src, dst.joints[i].name);
        if (si < 0) {
            continue;
        }
        const float ang = AngleBetweenDeg(
            BoneDirection(dst, w_dst, i),
            BoneDirection(src, w_src, static_cast<size_t>(si)));
        if (ang > 30.0f) {
            ++over30;
        }
        if (ang > max_dir) {
            max_dir = ang;
            worst = dst.joints[i].name;
        }
    }
    LOG(INFO) << "B. 直搬后骨段方向与源的最大差 = " << max_dir << "° (" << worst
              << ")，超过 30° 的骨 " << over30 << " 根";
    // 刻意断言"不对"：不作重定向时，多根骨的方向明显跑偏。
    CHECK_GT(max_dir, 60.0f)
        << "直搬属于对照组：应观察到明显朝向偏差（若这里失败，可能是有人把重定向"
           "塞进了 TransferPoseByNameNoRetarget —— 正式重定向属 M4 的另一个函数）";
    CHECK_GT(over30, 3) << "应有多根骨方向偏差 > 30°，实测 " << over30;
    LOG(INFO) << "OK TestNaiveTransferIsWrongOnRealAssets";
}

}  // namespace

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);
    TestMappingByName();
    TestEmptySourcePoseIsRest();
    TestNaiveTransferIsWrongOnRealAssets();
    LOG(INFO) << "skeleton_pose_transfer_test PASSED";
    return 0;
}
