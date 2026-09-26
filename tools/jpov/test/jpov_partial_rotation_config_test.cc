// JPOV 部位额外旋转（partial rotation）单测 —— 纯 CPU，不碰 GL。
//
// 被测（这套功能**只有两处接口**，与 thickness 同构）：
//   ① 骨架级配置：SkeletonManager(type, poses, thickness, partial_rotation_config)
//      —— 两个**关节名**的**合法性校验**（查不到 / 出现多次 / 两通道同指一骨 → LOG(FATAL)）；
//   ② 实例级取值：SkinnedInstanceState::partial_rotations 的默认值（全恒等）。
// 对应设计：interface/skeleton_types.h「部位额外旋转」+ docs/jpov_partial_rotation_design.md。
//
// ⚠️ 为什么"非法配置"的死亡测试**不需要 GL 上下文**：ctor 里名字→index 的校验排在所有 GL 调用
//   （glGenTextures…）**之前** ⇒ 非法配置在碰 GL 前就 LOG(FATAL) 了。这也是有意为之：
//   配置错误要最早崩，而不是"跑到一半才炸"。
// ⚠️ 合法配置的**正向**用例（子树波及范围对不对、纹理烘的对不对）本文件不测 —— 那条链要靠 GL
//   才能构造对象。纳入渲染/视觉验收（fbx viewer 的扭腰/仰头滑条 + 出图）。
//
// ⚠️ 避开恒真断言（见 skill: zero-run-code-reading-check）：默认值那条同时断言"xyzw = 0,0,0,1"
//   与"w 不是 0"（全 0 四元数会让 shader 的 normalize 出 NaN，是致命退化），两条不同时成立。

#include <array>
#include <string>
#include <vector>

#include <glog/logging.h>
#include <gtest/gtest.h>

#include "tools/jpov/interface/skeleton_types.h"
#include "tools/jpov/src/skeleton/skeleton_manager.h"

namespace jpov {
namespace {

// 3 骨玩具骨架：Hips(0) → Spine(1) → Head(2)
SkeletonType ToySkeleton() {
    SkeletonType t;
    auto add = [&](int parent, float len, const char* name) {
        SkeletonJoint j;
        j.parent = parent;
        j.rest_offset = {0.0f, len, 0.0f};
        j.name = name;
        t.joints.push_back(j);
        t.bind_rotation.push_back(geom::Quaternion<float>::Identity());
    };
    add(kSkeletonNoParent, 1.0f, "mixamorig:Hips");
    add(0, 0.5f, "mixamorig:Spine");
    add(1, 0.3f, "mixamorig:Head");
    t.Validate();
    return t;
}

std::vector<SkeletonPose> OnePose() { return {SkeletonPose::Identity(3)}; }

using PartialCfg = std::array<std::string, kNumPartialRotation>;

// ── 1. 每实例额外旋转：默认全恒等（0,0,0,1）──
TEST(PartialRotationConfigTest, InstanceRotationsDefaultToIdentity) {
    EXPECT_EQ(kNumPartialRotation, 2);
    const SkinnedInstanceState inst;
    ASSERT_EQ(inst.partial_rotations.size(), static_cast<size_t>(kNumPartialRotation));
    for (const geom::Quaternion<float>& q : inst.partial_rotations) {
        EXPECT_FLOAT_EQ(q.x, 0.0f);
        EXPECT_FLOAT_EQ(q.y, 0.0f);
        EXPECT_FLOAT_EQ(q.z, 0.0f);
        EXPECT_FLOAT_EQ(q.w, 1.0f);
    }
    // 非恒真判别：w 不能是 0（全 0 四元数 = 致命退化，shader normalize 出 NaN）。
    EXPECT_GT(inst.partial_rotations[kNumPartialRotation - 1].w, 0.5f);
}

// ── 2. 骨名查不到必须早崩 ──
TEST(PartialRotationConfigTest, UnknownJointNameIsFatal) {
    const SkeletonType t = ToySkeleton();
    PartialCfg cfg = {"mixamorig:NoSuchJoint", ""};
    EXPECT_DEATH(SkeletonManager(t, OnePose(), {}, cfg), "不在骨架里");
}

// ── 3. 两条通道指向同一根关节 = 歧义（两个旋转打架）必须早崩 ──
TEST(PartialRotationConfigTest, TwoChannelsSameJointIsFatal) {
    const SkeletonType t = ToySkeleton();
    PartialCfg cfg = {"mixamorig:Spine", "mixamorig:Spine"};
    EXPECT_DEATH(SkeletonManager(t, OnePose(), {}, cfg), "同一根关节");
}

// ── 4. 骨架里骨名重复（无法唯一定位通道）必须早崩 ──
TEST(PartialRotationConfigTest, DuplicateJointNameIsFatal) {
    SkeletonType t = ToySkeleton();
    // 再挂一根同名骨：Head 与它同名 "mixamorig:Spine"（父随便挂到 0）。
    SkeletonJoint dup;
    dup.parent = 0;
    dup.rest_offset = {0.0f, 0.2f, 0.0f};
    dup.name = "mixamorig:Spine";
    t.joints.push_back(dup);
    t.bind_rotation.push_back(geom::Quaternion<float>::Identity());
    t.Validate();
    PartialCfg cfg = {"mixamorig:Spine", ""};
    EXPECT_DEATH(SkeletonManager(t, OnePose(), {}, cfg), "出现多次");
}

}  // namespace
}  // namespace jpov
