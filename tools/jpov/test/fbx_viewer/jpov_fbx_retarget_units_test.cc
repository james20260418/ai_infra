// JPOV — 「长度单位只能在加载边界换算」契约测试（纯 CPU，不碰 GL）
//
// 背景（2026-09-20 实修的 bug，Danis 在 fbx viewer 里看到"蓝带皮飞走"）：
//   重定向的尺度因子是**无量纲骨长比**，只能做比例缩放、**不能做单位换算**。
//   旧行为下两个 loader 入口尺度不同：`LoadFbxAnimation` 把源单位（Mixamo = 厘米）原样
//   透传，而 `LoadFbxSkeleton` 已归一为米 ⇒ 比值静默错 100 倍：
//
//     实测 |root_offset| 最大 63.6 m  →  蓝骨人/蓝带皮直接飞出画面
//
//   定稿（单位铁律）：**换算只发生在加载边界** —— 两个入口都输出米，JPOV 内部不流通
//   厘米制数字，消费方（重定向 / 烘焙 / 火柴人 / 观察器）**不做任何换算**。
//
// 本测试钉住四件事：
//   ① 两个入口同尺度且确实是**米**（Hips 在 [0.5, 2.0] m）；
//   ② 源资产确实是厘米制（source_unit_meters ≈ 0.01）—— 否则"已换算"无从证明；
//   ③ 拿 clip 帧**直接**重定向（零换算）位移量级合理（< 2 m）——
//      这正是观察器的真实路径；一旦哪天又漏换算，这里立刻炸；
//   ④ **负向验证**：把帧当成"漏换算的旧行为"（×1/unit）⇒ 超量级（> 50 m），
//      证明门禁③真有区分力，不是恒真断言。
//
// 参考：docs/jpov_root_offset_design.md、interface/animation_clip.h（source_unit_meters 段）、
//      src/fbx_loader.h（「单位铁律」）。

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

    SkeletonType skeleton_m;
    FBXClip clip;
    CHECK(jpov::LoadFbxSkeleton(fbx, &skeleton_m)) << "LoadFbxSkeleton 失败: " << fbx;
    CHECK(jpov::LoadFbxAnimation(fbx, &clip)) << "LoadFbxAnimation 失败: " << fbx;
    std::vector<SkeletonType> skins;
    CHECK(jpov::LoadGltfSkeleton(glb, &skins)) << "LoadGltfSkeleton 失败: " << glb;
    CHECK(!skins.empty());
    const SkeletonType& glb_skel = skins[0];
    CHECK_GT(clip.frames.size(), 0u) << "源动画没有帧";

    // ---- ① 两个入口同尺度，且都是「米」----
    const int hips_m = FindBySuffix(skeleton_m, "Hips");
    const int hips_c = FindBySuffix(clip.skeleton, "Hips");
    CHECK_GE(hips_m, 0) << "LoadFbxSkeleton 产物应含 Hips";
    CHECK_GE(hips_c, 0) << "clip.skeleton 应含 Hips";
    const float h_m = skeleton_m.joints[static_cast<size_t>(hips_m)].rest_offset.y();
    const float h_c = clip.skeleton.joints[static_cast<size_t>(hips_c)].rest_offset.y();
    LOG(INFO) << "Hips 高度: LoadFbxSkeleton=" << h_m << " m  clip.skeleton=" << h_c << " m";
    // 人形 Hips 静息高在 [0.5, 2.0] m；cm 制会得 ≈104，米/厘米混算会得 ≈0.01。
    CHECK_GT(h_m, 0.5f) << "LoadFbxSkeleton 的 Hips 高 " << h_m << " 偏小，应已归一为米";
    CHECK_LT(h_m, 2.0f) << "LoadFbxSkeleton 的 Hips 高 " << h_m << " 偏大，疑似没换算";
    CHECK_GT(h_c, 0.5f) << "clip.skeleton 的 Hips 高 " << h_c
                        << " 偏小 —— 长度量必须在加载边界归一为米（旧 bug：这里原样透传 cm，"
                           "下游按无量纲骨长比缩放时会静默错 100 倍）";
    CHECK_LT(h_c, 2.0f) << "clip.skeleton 的 Hips 高 " << h_c << " 偏大，疑似没换算";
    CHECK_LT(std::fabs(h_c - h_m), 1e-3f)
        << "两个 loader 入口必须给出**同尺度**骨架（骨长逐骨相等的抽样）: " << h_c
        << " vs " << h_m;

    // ---- ② 源资产确实是厘米制（否则"换算过"无从证明）----
    LOG(INFO) << "source_unit_meters = " << clip.source_unit_meters;
    CHECK_LT(clip.source_unit_meters, 0.5f)
        << "Mixamo 源应为厘米（source_unit_meters≈0.01），实际 " << clip.source_unit_meters;
    CHECK_GT(clip.source_unit_meters, 0.0f) << "source_unit_meters 必须 >0";

    // ---- 两套 plan：源骨架用米制 skeleton_m（与 clip 帧同尺度）----
    const jpov::BodyRetargetPlan plan =
        jpov::BuildBodyRetargetPlan(glb_skel, skeleton_m);
    LOG(INFO) << "plan: 源骨盆高(米)=" << plan.source_leg_length
              << " 目标骨盆高(米)=" << plan.target_leg_length
              << " 尺度=" << plan.root_offset_scale
              << " Q_body=" << plan.q_body_angle_deg << "°";

    // 源位移确实非零（否则本测试无意义 —— 全是 0 的话什么断言都"过"）。
    float src_max = 0.0f;
    for (const SkeletonPose& p : clip.frames) {
        src_max = std::max(src_max, p.root_offset.Norm());
    }
    LOG(INFO) << "源（clip，已为米）|root_offset| 最大 = " << src_max << " m";
    CHECK_GT(src_max, 0.05f)
        << "源动画的 root_offset 几乎全 0（实测最大 " << src_max
        << " m）—— 取错了资产或不含 root-motion，本测试无区分力";
    CHECK_LT(src_max, 2.0f)
        << "源帧位移 " << src_max << " m 过大 —— loader 单位换算有问题？";

    // ---- ③ 观察器的真实路径：clip 帧**直接**重定向（零换算）----
    //   人跳舞的骨盆位移不该超过 ~2 米（目标骨盆高 0.53 m 的好几倍已很夸张）。
    const float fixed_max = MaxRetargetedOffsetNorm(plan, clip.frames);
    LOG(INFO) << "clip 帧直接重定向（零换算）: |root_offset| 最大 = " << fixed_max << " m";
    CHECK_LT(fixed_max, 2.0f)
        << "🔴 直接重定向后位移过大（" << fixed_max
        << " m）—— 蓝骨人会飞出画面。多为「加载边界没换算单位」（cm 被当米用）。";
    CHECK_GT(fixed_max, 0.01f)
        << "重定向后位移几乎为 0（" << fixed_max << " m）—— root_offset 没被搬过去？";

    // ---- ④ 负向验证：模拟"漏换算的旧行为"⇒ 超量级 ----
    {
        std::vector<SkeletonPose> buggy;
        buggy.reserve(clip.frames.size());
        const float inv = 1.0f / clip.source_unit_meters;  // 1/0.01 = 100
        for (const SkeletonPose& p : clip.frames) {
            SkeletonPose q = p;
            q.root_offset = q.root_offset * inv;  // cm 冒充米 = 旧行为
            buggy.push_back(q);
        }
        const float buggy_max = MaxRetargetedOffsetNorm(plan, buggy);
        LOG(INFO) << "漏换算（源单位直接给米制骨架）: |root_offset| 最大 = " << buggy_max
                  << " m";
        CHECK_GT(buggy_max, 50.0f)
            << "漏换算竟没造成超量级位移（" << buggy_max
            << " m）—— 「门禁③能抓住漏换算」这一前提不成立，需重新审视本测试";
        const float factor = buggy_max / fixed_max;
        LOG(INFO) << "放大倍数 ≈ " << factor << "（应≈ 1/source_unit_meters = " << inv
                  << "）";
        CHECK_GT(factor, 50.0f) << "放大倍数过小 —— 单位错配未被放大，前提可疑";
    }

    // ---- ⑤ 零入零出（纯原地动作 ⇒ 输出 0，向后兼容的根据）----
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
