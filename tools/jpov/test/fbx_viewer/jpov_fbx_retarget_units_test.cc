// JPOV — 「源帧长度量必须归一为米才能交给米制下游」契约测试（纯 CPU，不碰 GL）
//
// 背景（2026-09-20 实修的 bug）：
//   源帧来自 `LoadFbxAnimation`，按 loader「源单位原样透传」惯例，其 `root_offset` 是
//   **源单位**（Mixamo = 厘米）；而目标骨架 `glb`、观察器的 `skeleton_` 都是**米**。
//   旋转是无量纲的（怎么配都对），但 `root_offset` 是**长度量** —— 于是「重定向时用
//   骨盆高做尺度比」这一步，把 cm 的位移配到 m 的骨架上，比值静默错 100 倍：
//
//     实测 |root_offset| 最大 63.6 m  →  蓝骨人/蓝带皮直接飞出画面（Danis 实测报的）
//
//   修正：交给米制下游前，先把源帧的长度量乘 `clip.unit_meters` 归一为米。
//
// 本测试钉住三件事：
//   ① 陷阱真实存在（两入口单位不同：clip 是 cm、LoadFbxSkeleton 是 m）；
//   ② 归一后重定向的位移**合理**（量级与人身高相称）；
//   ③ **负向验证**：不归一就会超量级 —— 证明门禁②真有区分力（不是恒真断言）。
//
// 参考：docs/jpov_root_offset_design.md、interface/animation_clip.h 的 unit_meters 段。

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/interface/skeleton_retarget.h"
#include "tools/jpov/src/fbx_loader.h"
#include "tools/jpov/src/gltf_loader.h"
#include "tools/jpov/test/test_utils.h"

namespace {

using jpov::FBXClip;
using jpov::SkeletonPose;
using jpov::SkeletonType;
using jpov::Vec3f;

// 把源帧归一为米（与观察器 FbxViewerApp::NormalizePoseLengthsToMeters 同一语义）。
SkeletonPose InMeters(const SkeletonPose& raw, const FBXClip& clip) {
    SkeletonPose p = raw;
    p.root_offset = p.root_offset * clip.unit_meters;
    return p;
}

// 逐帧重定向，返回输出根位移的最大模长（米）。
float MaxRetargetedOffsetNorm(const jpov::BodyRetargetPlan& plan,
                              const std::vector<SkeletonPose>& poses) {
    float mx = 0.0f;
    for (const SkeletonPose& p : poses) {
        SkeletonPose out;
        jpov::BodyRetargetPose(plan, p, &out);
        mx = std::max(mx, out.root_offset.Norm());
    }
    return mx;
}

// 找骨名以 suffix 结尾的关节下标；找不到返回 -1。
int FindBySuffix(const SkeletonType& s, const std::string& suffix) {
    for (size_t i = 0; i < s.joints.size(); ++i) {
        const std::string& n = s.joints[i].name;
        if (n.size() >= suffix.size() &&
            n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

}  // namespace

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);
    const std::string dir = jpov::GetTestDataDir();
    const std::string fbx = dir + "/animations/hip_hop_dance.fbx";
    const std::string glb = dir + "/object3d/mixamo_male/mixamo_male.glb";

    SkeletonType skeleton_m;  // LoadFbxSkeleton → **米**
    FBXClip clip;             // LoadFbxAnimation → 源单位（cm）+ unit_meters
    CHECK(jpov::LoadFbxSkeleton(fbx, &skeleton_m)) << "LoadFbxSkeleton 失败: " << fbx;
    CHECK(jpov::LoadFbxAnimation(fbx, &clip)) << "LoadFbxAnimation 失败: " << fbx;
    std::vector<SkeletonType> skins;
    CHECK(jpov::LoadGltfSkeleton(glb, &skins)) << "LoadGltfSkeleton 失败: " << glb;
    CHECK(!skins.empty());
    const SkeletonType& glb_skel = skins[0];
    CHECK_GT(clip.frames.size(), 0u) << "源动画没有帧";

    // ---- ① 陷阱真实存在：两入口单位不同 ----
    const int hips_m = FindBySuffix(skeleton_m, "Hips");
    const int hips_c = FindBySuffix(clip.skeleton, "Hips");
    CHECK_GE(hips_m, 0) << "米制骨架应含 Hips";
    CHECK_GE(hips_c, 0) << "clip 骨架应含 Hips";
    const float h_m = skeleton_m.joints[static_cast<size_t>(hips_m)].rest_offset.y();
    const float h_c = clip.skeleton.joints[static_cast<size_t>(hips_c)].rest_offset.y();
    LOG(INFO) << "Hips 高度: skeleton_(米)=" << h_m << "  clip_.skeleton(源单位)=" << h_c
              << "  unit_meters=" << clip.unit_meters;
    CHECK_LT(clip.unit_meters, 0.5f)
        << "Mixamo 源应为厘米（unit_meters≈0.01），实际 " << clip.unit_meters;
    const float ratio_entries = std::fabs(h_c * clip.unit_meters - h_m);
    CHECK_LT(ratio_entries, 1e-3f)
        << "两入口应只差 unit_meters 这一个因子（否则陷阱不成立，本测试前提有误）: "
        << h_c * clip.unit_meters << " vs " << h_m;

    // ---- 两套 plan：源骨架用米制 skeleton_（与归一后的位姿同尺度）----
    const jpov::BodyRetargetPlan plan =
        jpov::BuildBodyRetargetPlan(glb_skel, skeleton_m);
    LOG(INFO) << "plan: 源骨盆高(米)=" << plan.source_leg_length
              << " 目标骨盆高(米)=" << plan.target_leg_length
              << " 尺度=" << plan.root_offset_scale
              << " Q_body=" << plan.q_body_angle_deg << "°";

    // 源帧（原始单位）与归一后（米）两份。
    std::vector<SkeletonPose> raw;
    raw.reserve(clip.frames.size());
    std::vector<SkeletonPose> meters;
    meters.reserve(clip.frames.size());
    for (const SkeletonPose& f : clip.frames) {
        raw.push_back(f);
        meters.push_back(InMeters(f, clip));
    }

    // 源位移确实非零（否则本测试无意义 —— 全是 0 的话什么断言都"过"）。
    float src_max = 0.0f;
    for (const SkeletonPose& p : meters) {
        src_max = std::max(src_max, p.root_offset.Norm());
    }
    CHECK_GT(src_max, 0.05f)
        << "源动画的 root_offset 几乎全 0（实测最大 " << src_max
        << " m）—— 取错了资产或不含 root-motion，本测试无区分力";

    // ---- ② 归一为米后：位移量级与人身高相称 ----
    //   人跳舞的骨盆位移不该超过 ~2 米（目标骨盆高 0.53 m 的好几倍已很夸张）。
    const float fixed_max = MaxRetargetedOffsetNorm(plan, meters);
    LOG(INFO) << "归一为米后: 重定向 |root_offset| 最大 = " << fixed_max << " m";
    CHECK_LT(fixed_max, 2.0f)
        << "🔴 归一后位移仍过大（" << fixed_max << " m）—— 单位链路又有问题";
    CHECK_GT(fixed_max, 0.01f)
        << "归一后位移几乎为 0（" << fixed_max << " m）—— root_offset 没被搬过去？";

    // ---- ③ 负向验证：忘记归一 → 超量级（证明门禁②有区分力）----
    const float buggy_max = MaxRetargetedOffsetNorm(plan, raw);
    LOG(INFO) << "忘记归一（源单位直接给米制骨架）: |root_offset| 最大 = " << buggy_max
              << " m";
    CHECK_GT(buggy_max, 50.0f)
        << "忘记归一竟没造成超量级位移（" << buggy_max
        << " m）—— 「门禁②能抓住忘归一」这一前提不成立，需重新审视本测试";
    // 两者之比应≈ 1/unit_meters（=100），这也直接体现"无量纲比值吃掉了单位差"。
    const float ratio = buggy_max / fixed_max;
    LOG(INFO) << "放大倍数 ≈ " << ratio << "（应≈ 1/unit_meters = "
              << 1.0f / clip.unit_meters << "）";

    // ---- ④ 零入零出（纯原地动作 ⇒ 输出 0，向后兼容的根据）----
    {
        SkeletonPose still = SkeletonPose::Identity(skeleton_m.bone_count());
        still.root_offset = Vec3f(0.0f, 0.0f, 0.0f);
        SkeletonPose out;
        jpov::BodyRetargetPose(plan, still, &out);
        CHECK_LT(out.root_offset.Norm(), 1e-6f)
            << "源不动 ⇒ 目标也不动（保持 bind 位置）";
    }

    LOG(INFO) << "jpov_fbx_retarget_units_test PASSED";
    return 0;
}
