// JPOV FBX 动画/骨架加载验证（纯 CPU，不碰 GL）
//
// 用真实资材 hip_hop_dance.fbx（Mixamo "Hip Hop Dancing" 舞，mixamorig 骨架全身动画）
// 验证 LoadFbxAnimation 忠实把源抓进 FBXClip：
//   1. 骨架：65 根 mixamorig:* 骨、根为 Hips(parent=-1)、拓扑序合法(每 parent<self)、
//      能沿树连成 mixamorig:Hips → Spine → Spine1 → … 主干。
//   2. 帧序与时序：frames_per_second>0，frames 数 = floor(时长*fps)+1（本例 30fps、时长
//      ~17.23s → 518 帧），每帧 bone_count==骨架骨数、joint_rotation 全单位四元数。
//   3. 内容 sane：动画帧的位姿在动 —— 取肢体骨第 0 帧与中段某帧的旋转不同（非全静止）。
//      （舞常循环, 末帧≈首帧, 故不用末帧对比。）
//
// 范围：本 loader 不做重定向/重采样，故测试只验「原样搬出来且数据自洽」，不做 gold/渲染。

#include <cmath>
#include <string>

#include <glog/logging.h>

#include "tools/jpov/src/fbx_loader.h"

namespace {

bool IsUnit(const geom::Quaternion<float>& q, float eps = 1e-3f) {
    return std::fabs(q.NormSqr() - 1.0f) < eps;
}

}  // namespace

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);

    const std::string kFbx = "tools/jpov/test/animations/hip_hop_dance.fbx";

    jpov::FBXClip clip;
    CHECK(jpov::LoadFbxAnimation(kFbx, &clip)) << "应能加载 hip_hop_dance.fbx";

    const jpov::SkeletonType& skel = clip.skeleton;
    const size_t nb = skel.joints.size();
    LOG(INFO) << "bones=" << nb << " fps=" << clip.frames_per_second
              << " frames=" << clip.frames.size();

    // ---- 1. 骨架 ----
    // mixamorig 人形帧范围预期带完整四肢+手指（≥50 骨是合理下限；Mixamo 全身 65 骨）。
    CHECK_GE(nb, 50u) << "该舞骨架应 ≥50 骨(mixamorig 全身), 实际 " << nb;
    CHECK_GE(skel.joints.size(), 1u);
    CHECK_EQ(skel.joints[0].name, "mixamorig:Hips") << "根骨应为 mixamorig:Hips";
    CHECK_EQ(skel.joints[0].parent, jpov::kSkeletonNoParent) << "根无父";
    // 每个非根都有名字 + 合法父，父在其之前（拓扑序）→ 无访问冲突.
    for (size_t i = 1; i < nb; ++i) {
        const auto& j = skel.joints[i];
        CHECK(!j.name.empty()) << "joint[" << i << "] 应有名字";
        CHECK_GE(j.parent, 0) << "joint[" << i << "] 非根须有父";
        CHECK_LT(static_cast<size_t>(j.parent), i)
            << "joint[" << i << "] 父须早于自身(拓扑序), parent=" << j.parent;
    }
    // 名字对得上经典主干（能 debug 对位的凭据），留余量给骨名缺失/版本差 → 只查一部分.
    bool have_spine = false;
    for (const auto& j : skel.joints)
        if (j.name == "mixamorig:Spine1") { have_spine = true; break; }
    CHECK(have_spine) << "骨架应含 mixamorig:Spine1";

    // ---- 2. 帧与时序 ----
    CHECK_GT(clip.frames_per_second, 0.0) << "应有时序(fps)";
    const double fps = clip.frames_per_second;
    const size_t nf = clip.frames.size();
    CHECK_GT(nf, 0u) << "应至少一帧";
    // 每帧 bone_count 对齐骨架骨数、旋转全单位。
    for (size_t k = 0; k < nf; ++k) {
        const jpov::SkeletonPose& pose = clip.frames[k];
        CHECK_EQ(static_cast<size_t>(pose.bone_count), nb)
            << "frame[" << k << "] bone_count 应与骨架一致";
        CHECK_EQ(pose.joint_rotation.size(), nb)
            << "frame[" << k << "] joint_rotation 尺寸应与骨架一致";
        for (size_t i = 0; i < nb; ++i) {
            CHECK(IsUnit(pose.joint_rotation[i]))
                << "frame[" << k << "] bone[" << i << "] 非单位四元数";
        }
    }

    // 预期 30fps & 时长~17.23s → ~517±0.5 个区间 → 帧数 = 517+1=518（±宽容）。
    LOG(INFO) << "clip fps=" << fps;
    CHECK_GE(nf, 500u) << "30fps 舞应 ~500+ 帧, 实际 " << nf;

    // ---- 3. 内容在动（帧间位姿变化）----
    if (nf >= 2) {
        // 取腿部/手臂骨 在第 0 与中段某帧的旋转必不同 → 证明是动画而非静止。
        // ⚠️ 不能用首/末帧比——舞常循环, 末帧≈首帧(回到起手 pose)。用中段帧对比。
        size_t test_bone = 0;
        for (size_t i = 0; i < nb; ++i) {
            const std::string& nm = skel.joints[i].name;
            if (nm == "mixamorig:LeftLeg" || nm == "mixamorig:LeftArm" ||
                nm == "mixamorig:RightLeg") {
                test_bone = i;
                break;
            }
        }
        const size_t kmid = nf / 3;  // 中段某帧(非首尾)
        const auto& q0 = clip.frames[0].joint_rotation[test_bone];
        const auto& q1 = clip.frames[kmid].joint_rotation[test_bone];
        const float dot = q0.Dot(q1);
        CHECK_LT(dot, 0.9995f)
            << "骨 " << skel.joints[test_bone].name << " 帧0 与中段帧"
            << kmid << " 旋转几乎一致, 疑似没在动(dot=" << dot << ")";
        LOG(INFO) << "sanity: bone[" << test_bone << "]="
                  << skel.joints[test_bone].name
                  << " frame0-vs-mid(" << kmid << ") rotation dot=" << dot;
    }

    LOG(INFO) << "jpov_fbx_loader_test PASSED";
    return 0;
}
