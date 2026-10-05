// JPOV 穿衣工具 — 软布自动蒙皮（weight transfer）纯函数单测
//
// 覆盖：重心插值（顶点精确命中 / 边中点 / 面内点）、top-K 剪枝与归一化、gap 兜底、
// 退化三角形在「三角形表」与「蒙皮表」两侧同步跳过、平滑降低总变差、kJoints 标记与
// 数组对齐。全部构造性输入（小网格 + 手写权重），无 GL / glTF。

#include "tools/jpov/clothing/weight_transfer.h"

#include <array>
#include <cmath>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "geom/3d/triangle_matcher_3d.h"
#include "tools/jpov/interface/mesh.h"

namespace jpov {
namespace clothing {
namespace {

// 造一个 3×2 的平面网格（y=0，x=0/1/2，z=0/1）：
//   v0=(0,0,0) v1=(1,0,0) v2=(2,0,0)
//   v3=(0,0,1) v4=(1,0,1) v5=(2,0,1)
//   三角形 (0,1,4)(0,4,3)(1,2,5)(1,5,4)
// 蒙皮：x=0 的顶点 → 骨 0；x=1 → 骨 1；x=2 → 骨 2（权重 1）。
MeshData MakeGridBody() {
    MeshData m;
    m.flags = static_cast<MeshVertexFlags>(static_cast<uint8_t>(MeshVertexFlags::kPosition) |
                                           static_cast<uint8_t>(MeshVertexFlags::kJoints));
    m.positions = {Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(2, 0, 0),
                   Vec3f(0, 0, 1), Vec3f(1, 0, 1), Vec3f(2, 0, 1)};
    m.indices = {0, 1, 4, 0, 4, 3, 1, 2, 5, 1, 5, 4};
    m.joint_indices.assign(6, {0, 0, 0, 0});
    m.joint_weights.assign(6, {0.0f, 0.0f, 0.0f, 0.0f});
    const int bone_of_x[3] = {0, 1, 2};
    const std::array<float, 3> xs = {0.0f, 1.0f, 2.0f};
    for (size_t i = 0; i < m.positions.size(); ++i) {
        int bone = 0;
        for (int c = 0; c < 3; ++c) {
            if (m.positions[i].x() == xs[static_cast<size_t>(c)]) {
                bone = bone_of_x[c];
            }
        }
        m.joint_indices[i] = {bone, 0, 0, 0};
        m.joint_weights[i] = {1.0f, 0.0f, 0.0f, 0.0f};
    }
    return m;
}

// 从 body mesh 建三角形 + 蒙皮表 + 匹配器（本测试的小尺度参数）。
struct BodyFixture {
    std::vector<Triangle3d> tris;
    BodySkinTable table;
    std::optional<geom::TriangleMatcher3d<double>> matcher;
};

BodyFixture BuildFixture(const MeshData& body, double local_distance = 0.1,
                         double grid = 0.1) {
    BodyFixture f;
    AppendMeshTriangles(body, &f.tris, &f.table);
    f.matcher.emplace(local_distance, grid, f.tris);
    return f;
}

// 一个位置处布料顶点（positions 单点）的 MeshData 骨架。
MeshData MakeCloth(const std::vector<Vec3f>& pts) {
    MeshData m;
    m.flags = MeshVertexFlags::kPosition;
    m.positions = pts;
    return m;
}

// 权重和。
float SumWeights(const std::array<float, 4>& w) {
    return w[0] + w[1] + w[2] + w[3];
}

// 找 joint 对应的权重（找不到返回 0）。
float WeightOfJoint(const std::array<int32_t, 4>& j,
                    const std::array<float, 4>& w, int32_t joint) {
    for (int k = 0; k < 4; ++k) {
        if (j[k] == joint && w[k] > 0.0f) {
            return w[k];
        }
    }
    return 0.0f;
}

}  // namespace

// 顶点精确落在身体某顶点上 → 完全拿到该顶点权重（无插值误差）。
TEST(WeightTransferTest, ExactBodyVertexCopiesWeights) {
    const MeshData body = MakeGridBody();
    BodyFixture f = BuildFixture(body);
    MeshData cloth = MakeCloth({Vec3f(1, 0, 0)});  // = v1（骨 1）
    const SkinTransferStats s =
        TransferSkinWeights(f.table, f.matcher.value(), &cloth, /*seed_eps*/0.005f,
                            /*weld*/kWeldToleranceM, /*max_inf*/4, /*growth*/0);
    EXPECT_EQ(s.vertex_count, 1u);
    EXPECT_EQ(s.non_seed_vertex_count, 0u);
    EXPECT_NEAR(WeightOfJoint(cloth.joint_indices[0], cloth.joint_weights[0], 1), 1.0f,
                1e-5f);
    EXPECT_NEAR(SumWeights(cloth.joint_weights[0]), 1.0f, 1e-5f);
}

// 顶点落在三角形内部（边中点）→ 重心插值：两骨各半。
TEST(WeightTransferTest, EdgeMidpointBlendsTwoBones) {
    const MeshData body = MakeGridBody();
    BodyFixture f = BuildFixture(body);
    // (0.5,0,0) 是三角形 (0,1,4) 的 a-b 边中点：骨0 / 骨1 各 0.5。
    MeshData cloth = MakeCloth({Vec3f(0.5f, 0.0f, 0.0f)});
    const SkinTransferStats s =
        TransferSkinWeights(f.table, f.matcher.value(), &cloth, /*seed_eps*/0.005f,
                            /*weld*/kWeldToleranceM, /*max_inf*/4, /*growth*/0);
    EXPECT_EQ(s.non_seed_vertex_count, 0u);
    EXPECT_NEAR(WeightOfJoint(cloth.joint_indices[0], cloth.joint_weights[0], 0), 0.5f,
                1e-4f);
    EXPECT_NEAR(WeightOfJoint(cloth.joint_indices[0], cloth.joint_weights[0], 1), 0.5f,
                1e-4f);
    EXPECT_NEAR(SumWeights(cloth.joint_weights[0]), 1.0f, 1e-5f);
}

// 面内点（三角形质心）→ 三骨均分。
TEST(WeightTransferTest, TriangleCentroidBlendsThreeBones) {
    const MeshData body = MakeGridBody();
    BodyFixture f = BuildFixture(body);
    // 三角形 (1,2,5) 质心 = ((1+2+2)/3, 0, (0+0+1)/3) = (5/3, 0, 1/3)：
    // 三角为 v1(骨1)、v2(骨2)、v5(骨2)；重心均分 → 骨1=1/3、骨2=2/3。
    const Vec3f c((1.0f + 2.0f + 2.0f) / 3.0f, 0.0f, (0.0f + 0.0f + 1.0f) / 3.0f);
    MeshData cloth = MakeCloth({c});
    const SkinTransferStats s =
        TransferSkinWeights(f.table, f.matcher.value(), &cloth, /*seed_eps*/0.05f,
                            /*weld*/kWeldToleranceM, /*max_inf*/4, /*growth*/0);
    EXPECT_EQ(s.non_seed_vertex_count, 0u);
    EXPECT_NEAR(WeightOfJoint(cloth.joint_indices[0], cloth.joint_weights[0], 1), 1.0f / 3.0f,
                1e-4f);
    EXPECT_NEAR(WeightOfJoint(cloth.joint_indices[0], cloth.joint_weights[0], 2), 2.0f / 3.0f,
                1e-4f);
    EXPECT_NEAR(SumWeights(cloth.joint_weights[0]), 1.0f, 1e-5f);
}

// top-K 剪枝：单三角形 3 角共 10 个不同骨 → 结果只保留 4 个、降序、归一化。
TEST(WeightTransferTest, TopKPrunesAndNormalizes) {
    MeshData body = MakeCloth({Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(0, 0, 1)});
    body.flags = static_cast<MeshVertexFlags>(static_cast<uint8_t>(MeshVertexFlags::kPosition) |
                                              static_cast<uint8_t>(MeshVertexFlags::kJoints));
    body.indices = {0, 1, 2};
    body.joint_indices = {{0, 1, 2, 3}, {4, 5, 6, 7}, {0, 4, 8, 9}};
    body.joint_weights = {{0.4f, 0.3f, 0.2f, 0.1f},
                          {0.4f, 0.3f, 0.2f, 0.1f},
                          {0.4f, 0.3f, 0.2f, 0.1f}};
    BodyFixture f = BuildFixture(body);
    const Vec3f centroid(1.0f / 3.0f, 0.0f, 1.0f / 3.0f);
    MeshData cloth = MakeCloth({centroid});
    const SkinTransferStats s =
        TransferSkinWeights(f.table, f.matcher.value(), &cloth, /*seed_eps*/0.05f,
                            /*weld*/kWeldToleranceM, /*max_inf*/4, /*growth*/0);
    EXPECT_EQ(s.non_seed_vertex_count, 0u);
    // 恰好 4 个非零（第 4 槽之后归零）。
    int nonzero = 0;
    for (int k = 0; k < 4; ++k) {
        if (cloth.joint_weights[0][k] > 0.0f) {
            ++nonzero;
        }
    }
    EXPECT_EQ(nonzero, 4);
    // 降序。
    for (int k = 1; k < 4; ++k) {
        EXPECT_GE(cloth.joint_weights[0][k - 1], cloth.joint_weights[0][k]);
    }
    // 归一化。
    EXPECT_NEAR(SumWeights(cloth.joint_weights[0]), 1.0f, 1e-5f);
    // 最大两项应是 joint0（0.4/3 + 0.4/3）与 joint4（0.4/3 + 0.3/3）。
    EXPECT_EQ(cloth.joint_indices[0][0], 0);
    EXPECT_EQ(cloth.joint_indices[0][1], 4);
    EXPECT_GT(cloth.joint_weights[0][0], cloth.joint_weights[0][1]);
}

// 非种子（离体）顶点：distance > seed_eps → 计入 non_seed；growth=0 时保留直接投影权重、仍归一化。
TEST(WeightTransferTest, NonSeedVertexKeepsDirectTransfer) {
    const MeshData body = MakeGridBody();
    BodyFixture f = BuildFixture(body, /*local_distance*/0.1, /*grid*/0.1);
    MeshData cloth = MakeCloth({Vec3f(1.0f, 5.0f, 0.5f)});  // 远离身体
    const SkinTransferStats s =
        TransferSkinWeights(f.table, f.matcher.value(), &cloth, /*seed_eps*/0.005f,
                            /*weld*/kWeldToleranceM, /*max_inf*/4, /*growth*/0);
    EXPECT_EQ(s.non_seed_vertex_count, 1u);
    EXPECT_EQ(s.seed_vertex_count, 0u);
    EXPECT_GT(s.max_body_distance_m, 4.0f);
    EXPECT_NEAR(SumWeights(cloth.joint_weights[0]), 1.0f, 1e-5f);
    // 候选为空 → 走线性兜底（也计入 no_candidate）。
    EXPECT_EQ(s.no_candidate_count, 1u);
}

// 退化三角形在「三角形表」与「蒙皮表」两侧同步跳过（两表恒同长）。
TEST(WeightTransferTest, DegenerateTriangleSkippedOnBothTables) {
    MeshData body = MakeCloth({Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(0, 0, 1)});
    body.flags = static_cast<MeshVertexFlags>(static_cast<uint8_t>(MeshVertexFlags::kPosition) |
                                              static_cast<uint8_t>(MeshVertexFlags::kJoints));
    // 第 2 个三角形 (3,4,5) 共线 = 退化。
    body.positions.push_back(Vec3f(0, 0, 2));
    body.positions.push_back(Vec3f(1, 0, 2));
    body.positions.push_back(Vec3f(2, 0, 2));
    body.joint_indices.assign(6, {0, 0, 0, 0});
    body.joint_weights.assign(6, {1.0f, 0.0f, 0.0f, 0.0f});
    body.indices = {0, 1, 2, 3, 4, 5};
    std::vector<Triangle3d> tris;
    BodySkinTable table;
    AppendMeshTriangles(body, &tris, &table);
    EXPECT_EQ(tris.size(), 1u);          // 退化面被跳过
    EXPECT_EQ(table.triangle_count(), 1u);  // 蒙皮表同进同出
}

// 生长：非种子顶点被两侧种子「长」成加权混合，种子冻结（种子=贴身，走人皮三点插值）。
// 三顶点链 0-1-2：0、2 贴身（种子，骨 0 / 骨 2），1 抬高 0.5m（非种子）。
TEST(WeightTransferTest, GrowthBlendsTowardSeedsAndFreezesSeeds) {
    const MeshData body = MakeGridBody();
    BodyFixture f = BuildFixture(body);
    MeshData cloth = MakeCloth({Vec3f(0, 0, 0), Vec3f(1, 0.5f, 0), Vec3f(2, 0, 0)});
    cloth.indices = {0, 1, 2};
    const SkinTransferStats s = TransferSkinWeights(
        f.table, f.matcher.value(), &cloth, /*seed_eps*/0.1f, /*weld*/kWeldToleranceM,
        /*max_inf*/4, /*growth*/50);
    EXPECT_EQ(s.seed_vertex_count, 2u);      // 顶点 0、2
    EXPECT_EQ(s.non_seed_vertex_count, 1u);  // 顶点 1
    // 种子冻结：顶点 0 仍是骨 0 100%。
    EXPECT_NEAR(WeightOfJoint(cloth.joint_indices[0], cloth.joint_weights[0], 0), 1.0f,
                1e-4f);
    // 非种子：夹在骨 0 / 骨 2 两种子之间 → 各半；原直接投影的骨 1 被抹掉。
    EXPECT_NEAR(WeightOfJoint(cloth.joint_indices[1], cloth.joint_weights[1], 0), 0.5f,
                1e-3f);
    EXPECT_NEAR(WeightOfJoint(cloth.joint_indices[1], cloth.joint_weights[1], 2), 0.5f,
                1e-3f);
    EXPECT_NEAR(WeightOfJoint(cloth.joint_indices[1], cloth.joint_weights[1], 1), 0.0f,
                1e-3f);
}

// 无种子的孤立顶点（不在任何三角形里 = deg 0）：生长不改变它 → 保留直接投影权重。
TEST(WeightTransferTest, SeedlessSingletonKeepsDirectTransfer) {
    const MeshData body = MakeGridBody();
    BodyFixture f = BuildFixture(body);
    // 0-1-2 连成三角形；顶点 3 离体且不在 indices 里 → 孤立分量、无种子。
    MeshData base = MakeCloth(
        {Vec3f(0, 0, 0), Vec3f(1, 0.5f, 0), Vec3f(2, 0, 0), Vec3f(1, 5, 0.5f)});
    base.indices = {0, 1, 2};
    MeshData no_grow = base;
    MeshData grow = base;
    TransferSkinWeights(f.table, f.matcher.value(), &no_grow, /*seed_eps*/0.1f,
                        /*weld*/kWeldToleranceM, /*max_inf*/4, /*growth*/0);
    TransferSkinWeights(f.table, f.matcher.value(), &grow, /*seed_eps*/0.1f,
                        /*weld*/kWeldToleranceM, /*max_inf*/4, /*growth*/50);
    EXPECT_EQ(grow.joint_indices[3], no_grow.joint_indices[3]);
    for (int k = 0; k < 4; ++k) {
        EXPECT_NEAR(grow.joint_weights[3][k], no_grow.joint_weights[3][k], 1e-6f);
    }
}

// 全是种子（都贴身）时生长不改变权重（种子全冻结）。
TEST(WeightTransferTest, GrowthKeepsUniformFieldWhenAllSeeds) {
    MeshData body = MakeGridBody();
    // 把所有身体顶点权重都改成骨 0（均匀场）。
    for (size_t i = 0; i < body.joint_indices.size(); ++i) {
        body.joint_indices[i] = {0, 0, 0, 0};
        body.joint_weights[i] = {1.0f, 0.0f, 0.0f, 0.0f};
    }
    BodyFixture f = BuildFixture(body);
    MeshData cloth = MakeCloth(body.positions);
    cloth.indices = body.indices;
    TransferSkinWeights(f.table, f.matcher.value(), &cloth, 0.005f, /*weld*/kWeldToleranceM, 4, /*growth*/2);
    for (size_t v = 0; v < cloth.positions.size(); ++v) {
        EXPECT_NEAR(WeightOfJoint(cloth.joint_indices[v], cloth.joint_weights[v], 0), 1.0f,
                    1e-5f);
        EXPECT_NEAR(SumWeights(cloth.joint_weights[v]), 1.0f, 1e-5f);
    }
}

// 结果置 kJoints flag，且 joint 数组长度 == 顶点数（供 GPU/保存使用）。
TEST(WeightTransferTest, SetsKJointsFlagAndAlignedArrays) {
    const MeshData body = MakeGridBody();
    BodyFixture f = BuildFixture(body);
    MeshData cloth = MakeCloth({Vec3f(0.5f, 0, 0), Vec3f(1.5f, 0, 0)});
    TransferSkinWeights(f.table, f.matcher.value(), &cloth, 0.005f, /*weld*/kWeldToleranceM, 4, 0);
    EXPECT_TRUE(MeshHasFlag(cloth.flags, MeshVertexFlags::kJoints));
    EXPECT_EQ(cloth.joint_indices.size(), cloth.positions.size());
    EXPECT_EQ(cloth.joint_weights.size(), cloth.positions.size());
}

// 焊接：位置完全重合 / 容差内重合的顶点并成一组；超出容差的不并。
TEST(WeightTransferTest, WeldMergesCoincidentKeepsDistinct) {
    const std::vector<Vec3f> pts = {
        Vec3f(0.0f, 0.0f, 0.0f),            // 0
        Vec3f(0.0f, 0.0f, 0.0f),            // 1 == 0（完全重合）
        Vec3f(1.0f, 0.0f, 0.0f),            // 2
        Vec3f(1.0f + 5.0e-5f, 0.0f, 0.0f),  // 3：离 2 仅 0.05mm（容差 0.1mm 内）
        Vec3f(2.0f, 0.0f, 0.0f),            // 4
        Vec3f(2.0f + 1.0e-2f, 0.0f, 0.0f),  // 5：离 4 1cm（远超容差）
    };
    const std::vector<int> rep = WeldVerticesByPosition(pts, kWeldToleranceM);
    EXPECT_EQ(rep[0], rep[1]);  // 完全重合 → 同组
    EXPECT_EQ(rep[2], rep[3]);  // 容差内 → 同组
    EXPECT_NE(rep[4], rep[5]);  // 超容差 → 不同组
    EXPECT_NE(rep[0], rep[2]);
    // 代表 = 组内最小下标。
    EXPECT_EQ(rep[0], 0);
    EXPECT_EQ(rep[1], 0);
    EXPECT_EQ(rep[2], 2);
    EXPECT_EQ(rep[3], 2);
    EXPECT_EQ(rep[4], 4);
    EXPECT_EQ(rep[5], 5);
}

// 焊接关闭：tolerance <= 0 → 恒等映射。
TEST(WeightTransferTest, WeldDisabledWhenNonPositive) {
    const std::vector<Vec3f> pts = {Vec3f(0, 0, 0), Vec3f(0, 0, 0)};
    const std::vector<int> rep = WeldVerticesByPosition(pts, /*tolerance*/ 0.0f);
    EXPECT_EQ(rep[0], 0);
    EXPECT_EQ(rep[1], 1);
}

// 🔴 开裂回归：缝合处被拆开的重复顶点，平滑后必须拿到**完全相同**的权重。
// 两块布片沿 x=1 缝合，但缝合线顶点各存两份（左边 L1/L3、右边 R0/R2 同位置）。
// 不焊接时：L1 只与左侧邻（骨0/1）平滑、R0 只与右侧邻（骨1/2）平滑 → 两侧发散 → 开裂。
TEST(WeightTransferTest, WeldKeepsSeamVerticesIdenticalAfterGrowth) {
    const MeshData body = MakeGridBody();
    BodyFixture f = BuildFixture(body);

    MeshData cloth = MakeCloth({
        Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(0, 0, 1), Vec3f(1, 0, 1),  // L0..L3
        Vec3f(1, 0, 0), Vec3f(2, 0, 0), Vec3f(1, 0, 1), Vec3f(2, 0, 1),  // R0..R3
    });
    cloth.indices = {0, 1, 3, 0, 3, 2, 4, 5, 7, 4, 7, 6};

    const SkinTransferStats s =
        TransferSkinWeights(f.table, f.matcher.value(), &cloth, /*seed_eps*/ 0.005f,
                            /*weld*/ kWeldToleranceM, /*max_inf*/ 4, /*growth*/ 2);

    // 缝合对：L1(1)↔R0(4)，L3(3)↔R2(6)。
    const std::pair<int, int> seams[2] = {std::make_pair(1, 4), std::make_pair(3, 6)};
    for (const std::pair<int, int>& seam : seams) {
        EXPECT_EQ(cloth.joint_indices[static_cast<size_t>(seam.first)],
                  cloth.joint_indices[static_cast<size_t>(seam.second)]);
        for (int k = 0; k < 4; ++k) {
            EXPECT_NEAR(cloth.joint_weights[static_cast<size_t>(seam.first)][k],
                        cloth.joint_weights[static_cast<size_t>(seam.second)][k], 1e-6f);
        }
        EXPECT_NEAR(SumWeights(cloth.joint_weights[static_cast<size_t>(seam.first)]),
                    1.0f, 1e-5f);
    }
    EXPECT_EQ(s.weld_merged_vertex_count, 2u);  // 两组重复（L1/R0、L3/R2）
    EXPECT_GE(s.growth_passes, 1u);  // 全为种子（贴身）→ 首轮即冻结、无变化
}

// 焊接容差可配：缝合线右侧整体偏移 0.5mm；容差 0.1mm 不并、2mm 并，并后两侧权重一致。
TEST(WeightTransferTest, WeldToleranceParameterControlsMerging) {
    const MeshData body = MakeGridBody();
    BodyFixture f = BuildFixture(body);
    MeshData cloth = MakeCloth({
        Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(0, 0, 1), Vec3f(1, 0, 1),  // L0..L3
        Vec3f(1.0005f, 0, 0), Vec3f(2, 0, 0), Vec3f(1.0005f, 0, 1),
        Vec3f(2, 0, 1),  // R0..R3（缝合线偏移 +0.5mm）
    });
    cloth.indices = {0, 1, 3, 0, 3, 2, 4, 5, 7, 4, 7, 6};

    MeshData fine = cloth;
    const SkinTransferStats s_fine = TransferSkinWeights(
        f.table, f.matcher.value(), &fine, /*seed_eps*/0.005f, /*weld*/1.0e-4f,
        /*max_inf*/4, /*growth*/2);
    EXPECT_EQ(s_fine.weld_merged_vertex_count, 0u);  // 0.5mm > 0.1mm → 不并

    MeshData coarse = cloth;
    const SkinTransferStats s_coarse = TransferSkinWeights(
        f.table, f.matcher.value(), &coarse, /*seed_eps*/0.005f, /*weld*/2.0e-3f,
        /*max_inf*/4, /*growth*/2);
    EXPECT_EQ(s_coarse.weld_merged_vertex_count, 2u);  // 0.5mm < 2mm → R0、R2 并入
    for (int k = 0; k < 4; ++k) {
        EXPECT_NEAR(coarse.joint_weights[1][k], coarse.joint_weights[4][k], 1e-6f);
        EXPECT_NEAR(coarse.joint_weights[3][k], coarse.joint_weights[6][k], 1e-6f);
    }
}

}  // namespace clothing
}  // namespace jpov
