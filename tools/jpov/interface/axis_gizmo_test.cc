// AxisGizmo 单测 —— 世界坐标轴辅助条带（三根 1m × 5cm 矩形带）
//
// 覆盖点（每条都对应一个"写错就看不出来"的失效模式）：
//   1. **轴向**：条带长轴必须落在指定坐标轴上（轴与宽度方向搞反 → 宽 5cm 的
//      那一边成了 1m，条带"转 90°"躺着，肉眼像坐标架散架）。
//   2. **起点贴 origin**：一端在 origin、另一端在 origin + axis × 1m
//      （不是以 origin 为中心）；另验证 origin 参数被正确带入。
//   3. **带宽 5cm**：且必须在**垂直轴向**的方向上量。
//   4. **三轴的朝向约定**（需求定档）：X 带法线朝 **+Z**、Y 带 **+X**、Z 带 **+X**。
//      该断言锁死在哪个平面里，防止绕序写反（法线背对相机 → 整条被裁掉看不见）。
//   5. **反转绕序必须反向法线** —— 编辑器靠"正反各画一次"实现双面可见，
//      这个性质不成立的话双面就是假的。
//   6. 颜色/规格常量（红绿蓝 + alpha 0.2 + 1m + 5cm）与需求一致。
//   7. 顶点顺序能构成合法 quad（4 顶点、两三角形、无自交；面积 = 长×宽）。

#include "tools/jpov/interface/axis_gizmo.h"

#include <algorithm>
#include <cmath>

#include <gtest/gtest.h>

namespace jpov {
namespace {

using Strip = std::array<Vec3f, 4>;

// 在给定方向 dir（须为单位向量）上投影全部顶点的取值范围。
struct ProjectionRange {
    float min_v;
    float max_v;
    float extent() const { return max_v - min_v; }
};

ProjectionRange ProjectAlong(const Strip& s, const Vec3f& dir) {
    float lo = 1e30f, hi = -1e30f;
    for (const Vec3f& p : s) {
        const float d = p.x()*dir.x() + p.y()*dir.y() + p.z()*dir.z();
        lo = std::min(lo, d);
        hi = std::max(hi, d);
    }
    return {lo, hi};
}


// 返回条带所在平面的法线方向（单位向量，取顶点两两叉积的归一化结果）。
// 退化（共线）时返回零向量。
Vec3f StripNormal(const Strip& s) {
    const Vec3f e1 = s[1] - s[0];
    const Vec3f e2 = s[2] - s[0];
    const Vec3f n(e1.y()*e2.z() - e1.z()*e2.y(),
                  e1.z()*e2.x() - e1.x()*e2.z(),
                  e1.x()*e2.y() - e1.y()*e2.x());
    const float len = std::sqrt(n.x()*n.x() + n.y()*n.y() + n.z()*n.z());
    if (len < 1e-8f) return Vec3f(0.0f, 0.0f, 0.0f);
    return Vec3f(n.x()/len, n.y()/len, n.z()/len);
}

// ==================== 1. 轴向：长轴落在指定轴上、宽度垂直 ====================

TEST(AxisStrip, LongAxisOnXWithWidthOnZ) {
    const Vec3f o(0.0f, 0.0f, 0.0f);
    const Strip s = MakeAxisStripVertices(o, Vec3f(1, 0, 0), Vec3f(0, 0, 1),
                                         kAxisGizmoLength, kAxisGizmoWidth);
    // 沿 +X 的跨度 = 条带长（1m）。
    EXPECT_NEAR(ProjectAlong(s, Vec3f(1, 0, 0)).extent(), 1.0f, 1e-5f);
    // 垂直轴向的两维：宽度方向是 5cm，第三维必须是**零厚**（单面）。
    EXPECT_NEAR(ProjectAlong(s, Vec3f(0, 0, 1)).extent(), 0.05f, 1e-5f);
    EXPECT_NEAR(ProjectAlong(s, Vec3f(0, 1, 0)).extent(), 0.0f, 1e-5f);
}

TEST(AxisStrip, LongAxisOnYWithWidthOnZ) {
    const Vec3f o(0.0f, 0.0f, 0.0f);
    const Strip s = MakeAxisStripVertices(o, Vec3f(0, 1, 0), Vec3f(0, 0, 1),
                                         kAxisGizmoLength, kAxisGizmoWidth);
    EXPECT_NEAR(ProjectAlong(s, Vec3f(0, 1, 0)).extent(), 1.0f, 1e-5f);
    EXPECT_NEAR(ProjectAlong(s, Vec3f(0, 0, 1)).extent(), 0.05f, 1e-5f);
    EXPECT_NEAR(ProjectAlong(s, Vec3f(1, 0, 0)).extent(), 0.0f, 1e-5f);
}

TEST(AxisStrip, LongAxisOnZWithWidthOnX) {
    const Vec3f o(0.0f, 0.0f, 0.0f);
    const Strip s = MakeAxisStripVertices(o, Vec3f(0, 0, 1), Vec3f(1, 0, 0),
                                         kAxisGizmoLength, kAxisGizmoWidth);
    EXPECT_NEAR(ProjectAlong(s, Vec3f(0, 0, 1)).extent(), 1.0f, 1e-5f);
    EXPECT_NEAR(ProjectAlong(s, Vec3f(1, 0, 0)).extent(), 0.05f, 1e-5f);
    EXPECT_NEAR(ProjectAlong(s, Vec3f(0, 1, 0)).extent(), 0.0f, 1e-5f);
}

// 非原点 origin 也要正确（编辑器把原点放在地面高度，不是 (0,0,0)）。
TEST(AxisStrip, OriginIsRespected) {
    const Vec3f o(0.0f, -3.0f, 0.0f);   // 地面高度
    const Strip s = MakeAxisStripVertices(o, Vec3f(1, 0, 0), Vec3f(0, 0, 1),
                                         1.0f, 0.05f);
    const ProjectionRange rx = ProjectAlong(s, Vec3f(1, 0, 0));
    EXPECT_NEAR(rx.min_v, 0.0f, 1e-5f);
    EXPECT_NEAR(rx.max_v, 1.0f, 1e-5f);
    const ProjectionRange ry = ProjectAlong(s, Vec3f(0, 1, 0));
    EXPECT_NEAR(ry.min_v, -3.0f, 1e-5f);
    EXPECT_NEAR(ry.max_v, -3.0f, 1e-5f);
}

// ==================== 2. 起点贴原点 ====================

TEST(AxisStrip, OneEndAtOriginOtherEndAtAxisTimesLength) {
    const Vec3f o(0.0f, 0.0f, 0.0f);
    const Strip s = MakeAxisStripVertices(o, Vec3f(0, 1, 0), Vec3f(0, 0, 1),
                                         1.0f, 0.05f);
    const ProjectionRange r = ProjectAlong(s, Vec3f(0, 1, 0));
    EXPECT_NEAR(r.min_v, 0.0f, 1e-5f);   // 一端贴原点
    EXPECT_NEAR(r.max_v, 1.0f, 1e-5f);   // 另一端 = +Y × 1m
    // 质心应在半长处（不是原点、不是终点）。
    Vec3f c(0, 0, 0);
    for (const Vec3f& p : s) c = c + p;
    c = Vec3f(c.x()/4.0f, c.y()/4.0f, c.z()/4.0f);
    EXPECT_NEAR(c.y(), 0.5f, 1e-5f);
    EXPECT_NEAR(c.x(), 0.0f, 1e-5f);
    EXPECT_NEAR(c.z(), 0.0f, 1e-5f);
}

// ==================== 3. 顶点顺序构成合法 quad ====================

TEST(AxisStrip, FourVerticesFormProperQuad) {
    const Strip s = MakeAxisStripVertices(Vec3f(0, 0, 0), Vec3f(1, 0, 0),
                                         Vec3f(0, 0, 1), 1.0f, 0.05f);
    // 四个顶点两两不重合（退化四边形会在屏幕上变成一条线/一个点）。
    for (int i = 0; i < 4; ++i) {
        for (int j = i + 1; j < 4; ++j) {
            const Vec3f d = s[i] - s[j];
            EXPECT_GT(std::sqrt(d.x()*d.x() + d.y()*d.y() + d.z()*d.z()), 1e-4f)
                << "顶点 " << i << " 与 " << j << " 重合";
        }
    }
    // 两对长边互相平行（|X| 相同的两顶点构成一组）—— 保证是平行四边形而非梯形。
    const Vec3f e_tail = s[1] - s[0];   // tail 侧宽度向量
    const Vec3f e_head = s[3] - s[2];   // head 侧宽度向量
    EXPECT_NEAR(e_tail.x(), e_head.x(), 1e-6f);
    EXPECT_NEAR(e_tail.y(), e_head.y(), 1e-6f);
    EXPECT_NEAR(e_tail.z(), e_head.z(), 1e-6f);
    // 面积 = 长 × 宽（用两条边的叉积模长算）。
    const Vec3f u = s[1] - s[0];       // 宽 5cm
    const Vec3f v = s[2] - s[0];       // 长 1m
    const Vec3f n(u.y()*v.z() - u.z()*v.y(),
                  u.z()*v.x() - u.x()*v.z(),
                  u.x()*v.y() - u.y()*v.x());
    EXPECT_NEAR(std::sqrt(n.x()*n.x() + n.y()*n.y() + n.z()*n.z()),
                0.05f * 1.0f, 1e-5f);
}

// ==================== 4. 三轴朝向约定（需求定档） ====================
//
// 🔑 这里断言的是**带符号**法线（不是 fabs）。原因：条带单面 + 开着背面裁剪，
//    法线背对相机就**整条消失** —— 即"法线朝哪一侧"是**功能性**的，不是审美。
//    实测踩过：统一用一种绕序时 X 带可见、Y/Z 带被剔除看不见。
// 朝向定档（Danis 2026-09-14 第二轮）：X 带法线 **+Z**、Y 带 **+X**、Z 带 **+X**。
TEST(AxisGizmoStrips, NormalsMatchRequirementSigned) {
    const AxisGizmoStrips g = MakeAxisGizmoStrips(Vec3f(0, 0, 0));

    // X 带：法线 = +Z
    const Vec3f nx = StripNormal(g.x);
    EXPECT_NEAR(nx.x(), 0.0f, 1e-5f);
    EXPECT_NEAR(nx.y(), 0.0f, 1e-5f);
    EXPECT_NEAR(nx.z(), 1.0f, 1e-5f) << "X 带法线应朝 +Z，实际="
                                     << nx.DebugString();

    // Y 带：法线 = +X
    const Vec3f ny = StripNormal(g.y);
    EXPECT_NEAR(ny.x(), 1.0f, 1e-5f) << "Y 带法线应朝 +X，实际="
                                     << ny.DebugString();
    EXPECT_NEAR(ny.y(), 0.0f, 1e-5f);
    EXPECT_NEAR(ny.z(), 0.0f, 1e-5f);

    // Z 带：法线 = +X
    const Vec3f nz = StripNormal(g.z);
    EXPECT_NEAR(nz.x(), 1.0f, 1e-5f) << "Z 带法线应朝 +X，实际="
                                     << nz.DebugString();
    EXPECT_NEAR(nz.y(), 0.0f, 1e-5f);
    EXPECT_NEAR(nz.z(), 0.0f, 1e-5f);
}

// 反转顶点顺序必须得到**方向相反**的法线（点积 = −1）—— 这是编辑器
// "正反各画一次实现双面可见"所依赖的性质。
// 若反转后法线没变（或退化），双面方案就是假的双面（背面仍然看不见）。
TEST(AxisGizmoStrips, ReversedWindingFlipsNormal) {
    const AxisGizmoStrips g = MakeAxisGizmoStrips(Vec3f(0, 0, 0));
    const std::array<Vec3f, 4>* strips[3] = {&g.x, &g.y, &g.z};
    for (int i = 0; i < 3; ++i) {
        const std::array<Vec3f, 4>& v = *strips[i];
        // 与 EditorApp::DrawAxisGizmo 用的反转形式一致：{v1, v0, v3, v2}
        const Strip rev = {v[1], v[0], v[3], v[2]};
        const Vec3f nf = StripNormal(v);
        const Vec3f nr = StripNormal(rev);
        EXPECT_NEAR(std::sqrt(nr.x()*nr.x() + nr.y()*nr.y() + nr.z()*nr.z()),
                    1.0f, 1e-5f) << "条带 " << i << " 反转后退化";
        const float dot = nf.x()*nr.x() + nf.y()*nr.y() + nf.z()*nr.z();
        EXPECT_NEAR(dot, -1.0f, 1e-5f)
            << "条带 " << i << " 反转顶点顺序后法线未反向（dot=" << dot << "）";
    }
}

TEST(AxisGizmoStrips, DimensionsMatchRequirement) {
    const AxisGizmoStrips g = MakeAxisGizmoStrips(Vec3f(0, 0, 0));

    // 三带长度均为 1m（需求：1m × 3）。
    EXPECT_NEAR(ProjectAlong(g.x, Vec3f(1, 0, 0)).extent(), 1.0f, 1e-5f);
    EXPECT_NEAR(ProjectAlong(g.y, Vec3f(0, 1, 0)).extent(), 1.0f, 1e-5f);
    EXPECT_NEAR(ProjectAlong(g.z, Vec3f(0, 0, 1)).extent(), 1.0f, 1e-5f);

    // 三带宽度均为 5cm（宽度展开方向：X 带沿 Y；Y 带沿 Z；Z 带沿 Y）。
    EXPECT_NEAR(ProjectAlong(g.x, Vec3f(0, 1, 0)).extent(), 0.05f, 1e-5f);
    EXPECT_NEAR(ProjectAlong(g.y, Vec3f(0, 0, 1)).extent(), 0.05f, 1e-5f);
    EXPECT_NEAR(ProjectAlong(g.z, Vec3f(0, 1, 0)).extent(), 0.05f, 1e-5f);

    // 反向自证：在"既非轴、也非宽度方向"的那一维上必须零厚（单面）。
    EXPECT_NEAR(ProjectAlong(g.x, Vec3f(0, 0, 1)).extent(), 0.0f, 1e-5f);
    EXPECT_NEAR(ProjectAlong(g.y, Vec3f(1, 0, 0)).extent(), 0.0f, 1e-5f);
    EXPECT_NEAR(ProjectAlong(g.z, Vec3f(1, 0, 0)).extent(), 0.0f, 1e-5f);
}

TEST(AxisGizmoStrips, OriginFollowsArgument) {
    const Vec3f o(0.0f, -2.5f, 0.0f);
    const AxisGizmoStrips g = MakeAxisGizmoStrips(o);
    // 三带都从 o 出发：沿各自轴的 min 投影 == o 在该轴上的分量。
    EXPECT_NEAR(ProjectAlong(g.x, Vec3f(1, 0, 0)).min_v, 0.0f, 1e-5f);
    EXPECT_NEAR(ProjectAlong(g.y, Vec3f(0, 1, 0)).min_v, -2.5f, 1e-5f);
    EXPECT_NEAR(ProjectAlong(g.z, Vec3f(0, 0, 1)).min_v, 0.0f, 1e-5f);
    // X 带没有厚度方向沿 Z（平面为 X-Y），故它在 Z 上完全落在 o.z = 0。
    EXPECT_NEAR(ProjectAlong(g.x, Vec3f(0, 0, 1)).min_v, 0.0f, 1e-5f);
    EXPECT_NEAR(ProjectAlong(g.x, Vec3f(0, 0, 1)).max_v, 0.0f, 1e-5f);
    // Z 带没在 X 上展开（平面为 Z-Y），故在 X 上完全落在 o.x = 0。
    EXPECT_NEAR(ProjectAlong(g.z, Vec3f(1, 0, 0)).min_v, 0.0f, 1e-5f);
    EXPECT_NEAR(ProjectAlong(g.z, Vec3f(1, 0, 0)).max_v, 0.0f, 1e-5f);
    // X 带所在的 y 范围 = o.y ± 半宽（宽度沿 Y 展开）—— 验证 origin 被正确带入。
    EXPECT_NEAR(ProjectAlong(g.x, Vec3f(0, 1, 0)).min_v, -2.5f - 0.025f, 1e-5f);
    EXPECT_NEAR(ProjectAlong(g.x, Vec3f(0, 1, 0)).max_v, -2.5f + 0.025f, 1e-5f);
}

// ==================== 5. 颜色 / 规格常量符合需求 ====================

TEST(AxisGizmoColors, RedGreenBlueWithAlphaPointTwo) {
    const Color cx = AxisGizmoColor(0);
    const Color cy = AxisGizmoColor(1);
    const Color cz = AxisGizmoColor(2);
    // X = 红
    EXPECT_FLOAT_EQ(cx.r, 1.0f); EXPECT_FLOAT_EQ(cx.g, 0.0f);
    EXPECT_FLOAT_EQ(cx.b, 0.0f);
    // Y = 绿
    EXPECT_FLOAT_EQ(cy.r, 0.0f); EXPECT_FLOAT_EQ(cy.g, 1.0f);
    EXPECT_FLOAT_EQ(cy.b, 0.0f);
    // Z = 蓝
    EXPECT_FLOAT_EQ(cz.r, 0.0f); EXPECT_FLOAT_EQ(cz.g, 0.0f);
    EXPECT_FLOAT_EQ(cz.b, 1.0f);
    // 三者的 alpha 都是 0.2（需求："alpha 还是 0.2"）。
    EXPECT_FLOAT_EQ(cx.a, 0.2f);
    EXPECT_FLOAT_EQ(cy.a, 0.2f);
    EXPECT_FLOAT_EQ(cz.a, 0.2f);
    // 颜色与颜色表一致（防两处定义分叉）。
    EXPECT_FLOAT_EQ(cx.r, kAxisGizmoColorX.r);
    EXPECT_FLOAT_EQ(cy.g, kAxisGizmoColorY.g);
    EXPECT_FLOAT_EQ(cz.b, kAxisGizmoColorZ.b);
}

TEST(AxisGizmoSpec, DefaultsMatchRequirement) {
    // 需求：1m 长 + 带宽 5cm + alpha 0.2。
    EXPECT_FLOAT_EQ(kAxisGizmoLength, 1.0f);
    EXPECT_FLOAT_EQ(kAxisGizmoWidth, 0.05f);
    EXPECT_FLOAT_EQ(kAxisGizmoColorX.a, 0.2f);
    // 默认参数即需求值（改默认值必须同步改需求 → 这条会拦住）。
    const Strip s = MakeAxisStripVertices(Vec3f(0, 0, 0), Vec3f(0, 1, 0),
                                         Vec3f(0, 0, 1));
    EXPECT_NEAR(ProjectAlong(s, Vec3f(0, 1, 0)).extent(), 1.0f, 1e-5f);
    EXPECT_NEAR(ProjectAlong(s, Vec3f(0, 0, 1)).extent(), 0.05f, 1e-5f);
}

// 轴名（编辑器说明文字用）。
TEST(AxisGizmoName, ReturnsXYZ) {
    EXPECT_STREQ(AxisGizmoName(0), "X");
    EXPECT_STREQ(AxisGizmoName(1), "Y");
    EXPECT_STREQ(AxisGizmoName(2), "Z");
}

}  // namespace
}  // namespace jpov
