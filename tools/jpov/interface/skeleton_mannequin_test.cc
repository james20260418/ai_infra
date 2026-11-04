// JPOV Skeleton — 程序化蓝人模特网格单测（纯 CPU）。
//
// 断言目标：
//   1. 网格非空、索引是三角形、关节索引都在骨架范围内；
//   2. 每顶点权重和 == 1（刚性权重，LBS 不会整体缩放）；
//   3. 顶点在骨架空间里覆盖完整人身高（不是塌在原点/只有一根杆）；
//   4. 所有顶点有限（没有 NaN/Inf）。

#include <cmath>
#include <set>

#include <gtest/gtest.h>

#include "tools/jpov/interface/mixamo23_skeleton.h"
#include "tools/jpov/interface/skeleton_mannequin.h"

namespace jpov {
namespace {

TEST(SkeletonMannequinTest, MeshIsCompleteAndRigidlyWeighted) {
    const SkeletonType skeleton = Mixamo23Skeleton(1.75f);
    const MeshData mesh = BuildMannequinMeshInBoneSpace(skeleton, 1.0f);

    ASSERT_FALSE(mesh.positions.empty());
    EXPECT_TRUE(MeshHasFlag(mesh.flags, MeshVertexFlags::kPosition));
    EXPECT_TRUE(MeshHasFlag(mesh.flags, MeshVertexFlags::kNormal));
    EXPECT_TRUE(MeshHasFlag(mesh.flags, MeshVertexFlags::kJoints));
    ASSERT_EQ(mesh.normals.size(), mesh.positions.size());
    ASSERT_EQ(mesh.joint_indices.size(), mesh.positions.size());
    ASSERT_EQ(mesh.joint_weights.size(), mesh.positions.size());
    ASSERT_EQ(mesh.indices.size() % 3, 0u);
    EXPECT_GT(mesh.indices.size(), 3000u);

    float min_y = mesh.positions[0].y();
    float max_y = mesh.positions[0].y();
    std::set<int> used_joints;
    for (size_t i = 0; i < mesh.positions.size(); ++i) {
        const Vec3f& p = mesh.positions[i];
        EXPECT_TRUE(p.IsFinite()) << "vertex " << i;
        min_y = std::min(min_y, p.y());
        max_y = std::max(max_y, p.y());
        const auto& weights = mesh.joint_weights[i];
        EXPECT_NEAR(weights[0] + weights[1] + weights[2] + weights[3], 1.0f, 1e-5f);
        for (int k = 0; k < 4; ++k) {
            const int joint = mesh.joint_indices[i][static_cast<size_t>(k)];
            EXPECT_GE(joint, 0);
            EXPECT_LT(joint, skeleton.bone_count());
            if (weights[static_cast<size_t>(k)] > 0.0f) {
                used_joints.insert(joint);
            }
        }
    }
    EXPECT_GT(max_y - min_y, 1.4f);   // 1.75m 人台不能只剩一条杆
    EXPECT_NEAR(min_y, 0.0f, 0.12f);  // 脚附近接近 y=0（球体半径会略低于 0）
    // 至少覆盖躯干 + 四肢 + 头：23 骨里叶子骨也要有权重（否则手/脚是空的）。
    EXPECT_GE(used_joints.size(), 18u);
}

}  // namespace
}  // namespace jpov
