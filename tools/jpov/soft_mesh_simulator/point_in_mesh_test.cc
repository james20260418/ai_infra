// JPOV 「点是否在网格体内」查询的单测（纯 CPU / GL-free）
//
// 锁定 IsPointInMesh 的契约：
//   - 凸多面体（轴对齐盒 / 八面体）：体内 → true，体外 → false；贴面两侧正确
//   - 带空腔的封闭壳（内外两层盒）：空腔 → false，壳层 → true（奇偶法能穿透）
//   - 旋转 + 平移的盒：按**真实体**判，而非拿 AABB 糊弄
//     （短轴外、但仍在 AABB 内的点必须判为体外）
//   - 索引网格与非索引网格两条路径结果一致
//   - 纯查询：不改动 mesh
//   - 前置条件：无完整三角形 / 索引数非 3 倍数 / 索引越界 → LOG(FATAL)
//
// 断言均可区分对错（非恒真）：把任一处 inside/outside 期望翻反，测试立即 FAIL。

#include "tools/jpov/soft_mesh_simulator/point_in_mesh.h"

#include <cstdint>
#include <random>
#include <vector>

#include <glog/logging.h>
#include <gtest/gtest.h>

#include "geom/common/vec.h"
#include "tools/jpov/interface/mesh.h"

namespace {

using jpov::MeshData;
using jpov::MeshVertexFlags;
using jpov::soft_mesh_simulator::IsPointInMesh;
using Vec3f = geom::Vec3<float>;

// 便捷：按分量构造查询点。
Vec3f P(float x, float y, float z) {
    return Vec3f(x, y, z);
}

// 正八面体（体内 = |x|+|y|+|z| <= 1 的钻石体）：6 顶点 / 8 三角形，封闭。
// 覆盖「面非轴对齐、非盒」的一般封闭体。各边恰好被两个三角形共享（封闭）。
MeshData MakeOctahedron() {
    MeshData m;
    m.flags = MeshVertexFlags::kPosition;
    m.positions = {
        { 1.0f,  0.0f,  0.0f},  // 0
        {-1.0f,  0.0f,  0.0f},  // 1
        { 0.0f,  1.0f,  0.0f},  // 2
        { 0.0f, -1.0f,  0.0f},  // 3
        { 0.0f,  0.0f,  1.0f},  // 4
        { 0.0f,  0.0f, -1.0f},  // 5
    };
    m.indices = {0, 2, 4,  2, 1, 4,  1, 3, 4,  3, 0, 4,
                 2, 0, 5,  1, 2, 5,  3, 1, 5,  0, 3, 5};
    m.Validate();
    return m;
}

// 仅取位置、去掉法线的盒（MakeBox 带法线；这里统一成 kPosition，
// 便于与「合并 / 非索引」用例混用同一种网格）。半宽含义同 MakeBox。
MeshData MakePositionBox(float front_half, float up_half, float left_half) {
    const MeshData src = MeshData::MakeBox(front_half, up_half, left_half);
    MeshData m;
    m.flags = MeshVertexFlags::kPosition;
    m.positions = src.positions;
    m.indices = src.indices;
    m.Validate();
    return m;
}

// 把「索引网格」展开成「非索引网格」（每三角形 3 个顶点顺序铺开），
// 用于验证两条解释路径等价。
MeshData ToNonIndexed(const MeshData& src) {
    CHECK(!src.indices.empty()) << "ToNonIndexed: 源网格必须带索引";
    MeshData m;
    m.flags = MeshVertexFlags::kPosition;
    for (uint32_t idx : src.indices) {
        m.positions.push_back(src.positions[idx]);
    }
    m.Validate();
    return m;
}

// 合并两个仅含位置的封闭网格为一个网格（b 的索引整体重基），
// 用于构造带空腔的壳：实心 = 外壳内部 − 内壳内部。
MeshData MergePositionMeshes(const MeshData& a, const MeshData& b) {
    CHECK(!a.indices.empty() && !b.indices.empty())
        << "MergePositionMeshes: 两个源网格都必须带索引";
    MeshData m;
    m.flags = MeshVertexFlags::kPosition;
    m.positions = a.positions;
    m.positions.insert(m.positions.end(), b.positions.begin(), b.positions.end());
    m.indices = a.indices;
    const uint32_t base = static_cast<uint32_t>(a.positions.size());
    for (uint32_t idx : b.indices) {
        m.indices.push_back(base + idx);
    }
    m.Validate();
    return m;
}

// ---------------------------------------------------------------- 凸多面体 ---

TEST(PointInMeshTest, AxisAlignedBoxInteriorIsInside) {
    const MeshData box = MakePositionBox(/*front_half=*/1.0f, /*up_half=*/1.0f,
                                         /*left_half=*/1.0f);
    EXPECT_TRUE(IsPointInMesh(P(0.0f, 0.0f, 0.0f), box));
    EXPECT_TRUE(IsPointInMesh(P(0.5f, 0.5f, 0.5f), box));
    EXPECT_TRUE(IsPointInMesh(P(-0.9f, 0.9f, -0.9f), box));
}

TEST(PointInMeshTest, AxisAlignedBoxOutsideIsOutside) {
    const MeshData box = MakePositionBox(1.0f, 1.0f, 1.0f);
    EXPECT_FALSE(IsPointInMesh(P(5.0f, 5.0f, 5.0f), box));
    EXPECT_FALSE(IsPointInMesh(P(1.01f, 0.0f, 0.0f), box));
    EXPECT_FALSE(IsPointInMesh(P(0.0f, -1.01f, 0.0f), box));
    EXPECT_FALSE(IsPointInMesh(P(0.0f, 0.0f, 1.01f), box));
    // 贴近面但仍在体内（0.99 < 1）。
    EXPECT_TRUE(IsPointInMesh(P(0.0f, 0.0f, 0.99f), box));
}

TEST(PointInMeshTest, NonUniformBoxNearCorner) {
    // 半宽：X(left)=1、Y(up)=0.5、Z(front)=2 → (0.9,0.4,1.9) 在内；
    // (1.1,0,0)、(0,0.6,0) 在外。
    const MeshData box = MakePositionBox(/*front_half=*/2.0f, /*up_half=*/0.5f,
                                         /*left_half=*/1.0f);
    EXPECT_TRUE(IsPointInMesh(P(0.9f, 0.4f, 1.9f), box));
    EXPECT_FALSE(IsPointInMesh(P(1.1f, 0.0f, 0.0f), box));
    EXPECT_FALSE(IsPointInMesh(P(0.0f, 0.6f, 0.0f), box));
}

TEST(PointInMeshTest, OctahedronInteriorAndExterior) {
    const MeshData oct = MakeOctahedron();  // 体内：|x|+|y|+|z| <= 1
    EXPECT_TRUE(IsPointInMesh(P(0.0f, 0.0f, 0.0f), oct));
    EXPECT_TRUE(IsPointInMesh(P(0.2f, 0.2f, 0.2f), oct));    // 0.6 < 1
    EXPECT_TRUE(IsPointInMesh(P(0.9f, 0.02f, 0.02f), oct));  // 0.94 < 1
    EXPECT_FALSE(IsPointInMesh(P(0.5f, 0.5f, 0.5f), oct));   // 1.5 > 1
    EXPECT_FALSE(IsPointInMesh(P(0.6f, 0.3f, 0.2f), oct));   // 1.1 > 1
    EXPECT_FALSE(IsPointInMesh(P(2.0f, 0.0f, 0.0f), oct));
}

// ------------------------------------------------------------ 带空腔的壳 ---

TEST(PointInMeshTest, NestedBoxShellHasCavity) {
    // 外壳半宽 1，内壳半宽 0.5：实心 = 两壳之间的一层。
    const MeshData shell = MergePositionMeshes(
        MakePositionBox(1.0f, 1.0f, 1.0f),
        MakePositionBox(0.5f, 0.5f, 0.5f));

    // 空腔（内壳内部）→ 在外：射线穿出内壳 + 外壳 = 2 次（偶）。
    EXPECT_FALSE(IsPointInMesh(P(0.0f, 0.0f, 0.0f), shell));
    EXPECT_FALSE(IsPointInMesh(P(0.0f, 0.0f, 0.2f), shell));
    // 壳层（内壳外、外壳内）→ 在内：射线只穿出外壳 = 1 次（奇）。
    EXPECT_TRUE(IsPointInMesh(P(0.75f, 0.0f, 0.0f), shell));
    EXPECT_TRUE(IsPointInMesh(P(0.0f, -0.75f, 0.0f), shell));
    EXPECT_TRUE(IsPointInMesh(P(0.0f, 0.0f, 0.75f), shell));
    // 外壳之外 → 在外。
    EXPECT_FALSE(IsPointInMesh(P(2.0f, 0.0f, 0.0f), shell));
}

// ------------------------------------------------ 旋转 / 平移盒（非轴对齐）---

TEST(PointInMeshTest, RotatedTranslatedBoxUsesTrueSolidNotAabb) {
    // 局部轴：+Z(front) 沿世界 (1,0,1)/√2（长轴，半长 2）；
    //         +X(left)  沿世界 (1,0,-1)/√2（短轴，半长 0.2）；
    //         +Y(up)    沿世界 (0,1,0)    （半长 1）。
    const Vec3f center(3.0f, 1.0f, -2.0f);
    const MeshData box = MeshData::MakeOrientedBox(
        /*front_half_width=*/2.0f, /*up_half_width=*/1.0f,
        /*left_half_width=*/0.2f,
        /*up=*/Vec3f(0.0f, 1.0f, 0.0f),
        /*front=*/Vec3f(1.0f, 0.0f, 1.0f),
        /*translation=*/center);

    const float s = 0.70710678f;            // 1/√2
    const Vec3f long_axis(s, 0.0f, s);      // 长轴单位向量
    const Vec3f short_axis(s, 0.0f, -s);    // 短轴单位向量
    const Vec3f up_axis(0.0f, 1.0f, 0.0f);

    // 沿长轴：1.5 < 2 在内；2.5 > 2 在外。
    EXPECT_TRUE(IsPointInMesh(center + long_axis * 1.5f, box));
    EXPECT_FALSE(IsPointInMesh(center + long_axis * 2.5f, box));
    // 沿短轴：0.15 < 0.2 在内；0.5 > 0.2 在外——注意 0.5 仍落在盒的 AABB 内，
    // 所以这一条专门用来戳穿「用 AABB 蒙混」的错误实现。
    EXPECT_TRUE(IsPointInMesh(center + short_axis * 0.15f, box));
    EXPECT_FALSE(IsPointInMesh(center + short_axis * 0.5f, box));
    // 沿 up：0.5 < 1 在内；1.5 > 1 在外。
    EXPECT_TRUE(IsPointInMesh(center + up_axis * 0.5f, box));
    EXPECT_FALSE(IsPointInMesh(center + up_axis * 1.5f, box));
    // 世界原点离它很远 → 在外。
    EXPECT_FALSE(IsPointInMesh(P(0.0f, 0.0f, 0.0f), box));
}

// ---------------------------------------------------- 索引 / 非索引 等价 ---

// 索引网格与非索引网格（同一几何，两种存储）必须给出**逐点相同**的判定。
//
// 只在几个手挑点上比会漏掉「非索引三角形分组写错」这类 bug——本用例因此除显式
// 样点外，还做一轮**确定性随机扫描**（固定种子，2000 点）逐点核对两条路径。
// 网格取真几何上下文：八面体（一般朝向面）与盒（轴对齐面）各扫一遍。
TEST(PointInMeshTest, NonIndexedPathMatchesIndexedPath) {
    const MeshData oct_indexed = MakeOctahedron();
    const MeshData oct_non_indexed = ToNonIndexed(oct_indexed);
    const MeshData box_indexed = MakePositionBox(1.0f, 1.0f, 1.0f);
    const MeshData box_non_indexed = ToNonIndexed(box_indexed);

    // 显式样点（含已知内/外），先钉住基线。
    const Vec3f samples[] = {
        P(0.0f, 0.0f, 0.0f), P(0.2f, 0.2f, 0.2f), P(0.9f, 0.02f, 0.02f),
        P(0.5f, 0.5f, 0.5f), P(0.6f, 0.3f, 0.2f), P(2.0f, 0.0f, 0.0f),
    };
    for (const Vec3f& pt : samples) {
        EXPECT_EQ(IsPointInMesh(pt, oct_indexed), IsPointInMesh(pt, oct_non_indexed))
            << "八面体：点 (" << pt.x() << "," << pt.y() << "," << pt.z()
            << ") 在索引/非索引两条路径下结果不一致";
        EXPECT_EQ(IsPointInMesh(pt, box_indexed), IsPointInMesh(pt, box_non_indexed))
            << "盒：点 (" << pt.x() << "," << pt.y() << "," << pt.z()
            << ") 在索引/非索引两条路径下结果不一致";
    }

    // 确定性随机扫描：覆盖体内 / 体外 / 贴面附近的广域，逼出任何「分组/步长写错」。
    std::mt19937 rng(20260927u);
    std::uniform_real_distribution<float> coord(-1.4f, 1.4f);
    for (int i = 0; i < 2000; ++i) {
        const Vec3f pt(coord(rng), coord(rng), coord(rng));
        EXPECT_EQ(IsPointInMesh(pt, oct_indexed), IsPointInMesh(pt, oct_non_indexed))
            << "八面体随机扫描不一致：(" << pt.x() << "," << pt.y() << "," << pt.z() << ")";
        EXPECT_EQ(IsPointInMesh(pt, box_indexed), IsPointInMesh(pt, box_non_indexed))
            << "盒随机扫描不一致：(" << pt.x() << "," << pt.y() << "," << pt.z() << ")";
    }
}

TEST(PointInMeshTest, QueryDoesNotModifyMesh) {
    const MeshData box = MakePositionBox(1.0f, 1.0f, 1.0f);
    const std::vector<Vec3f> before = box.positions;
    (void)IsPointInMesh(P(0.0f, 0.0f, 0.0f), box);
    (void)IsPointInMesh(P(9.0f, 0.0f, 0.0f), box);
    EXPECT_EQ(box.positions.size(), before.size());
    for (size_t i = 0; i < before.size(); ++i) {
        EXPECT_EQ(box.positions[i], before[i]);
    }
}

// -------------------------------------------------------------- 前置条件 ---

TEST(PointInMeshTest, DeathWhenNoCompleteTriangle) {
    MeshData m;
    m.flags = MeshVertexFlags::kPosition;
    m.positions = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}};  // 只有 2 个顶点
    m.Validate();
    EXPECT_DEATH(IsPointInMesh(P(0.0f, 0.0f, 0.0f), m), "IsPointInMesh");
}

TEST(PointInMeshTest, DeathWhenIndexCountNotMultipleOfThree) {
    MeshData m;
    m.flags = MeshVertexFlags::kPosition;
    m.positions = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    m.indices = {0, 1, 2, 0};  // 4 个索引，不是 3 的倍数
    m.Validate();
    EXPECT_DEATH(IsPointInMesh(P(0.0f, 0.0f, 0.0f), m), "IsPointInMesh");
}

TEST(PointInMeshTest, DeathWhenIndexOutOfRange) {
    MeshData m;
    m.flags = MeshVertexFlags::kPosition;
    m.positions = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    m.indices = {0, 1, 7};  // 7 越界
    m.Validate();
    EXPECT_DEATH(IsPointInMesh(P(0.0f, 0.0f, 0.0f), m), "IsPointInMesh");
}

}  // namespace
