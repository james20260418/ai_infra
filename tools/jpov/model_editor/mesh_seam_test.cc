// mesh_seam_test.cc — 断面缝合纯函数单测（合成网格 + 真资产端到端在 stitch CLI 里验）
//
// 覆盖：
//   ① 三种算法都能把 target 的底口雕成 reference 的顶口（位置 + 形状）；
//   ② 环重建后 target 断面 == reference 环（逐点、同顶点数）⇒ 可 1:1 焊接；
//   ③ 影响带外（远高于断面）的顶点**一动不动**；
//   ④ 非环断面（两根管 = 两个环）被拒绝；
//   ⑤ 切空的平面被拒绝；
//   ⑥ 圆度指标：缝合后断面半径 = reference 半径（不是恒真断言，是真量出来的）。

#include "tools/jpov/model_editor/mesh_seam.h"

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

namespace {

using jpov::MeshData;
using jpov::MeshVertexFlags;
using jpov::Vec3f;
using jpov::model_editor::SeamAlgo;
using jpov::model_editor::SeamOptions;
using jpov::model_editor::SeamStats;

constexpr float kPi = 3.14159265358979323846f;

// 开口圆管（上下无盖）：seg 段 × rings 圈，轴向沿 +Y。
// 开口在两端 —— 顺带验证「既有开口不参与断面环」。
MeshData MakeTube(float radius, float y0, float y1, int seg, int rings,
                  float center_x = 0.0f, float center_z = 0.0f) {
    MeshData mesh;
    mesh.flags = static_cast<MeshVertexFlags>(
        static_cast<uint8_t>(MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(MeshVertexFlags::kNormal));
    for (int r = 0; r <= rings; ++r) {
        const float t = static_cast<float>(r) / rings;
        const float y = y0 + (y1 - y0) * t;
        for (int s = 0; s < seg; ++s) {
            const float a = 2.0f * kPi * static_cast<float>(s) / seg;
            const float nx = std::cos(a);
            const float nz = std::sin(a);
            mesh.positions.push_back(
                Vec3f(center_x + radius * nx, y, center_z + radius * nz));
            mesh.normals.push_back(Vec3f(nx, 0.0f, nz));
        }
    }
    for (int r = 0; r < rings; ++r) {
        for (int s = 0; s < seg; ++s) {
            const uint32_t a = static_cast<uint32_t>(r * seg + s);
            const uint32_t b = static_cast<uint32_t>(r * seg + (s + 1) % seg);
            const uint32_t c = static_cast<uint32_t>((r + 1) * seg + s);
            const uint32_t d = static_cast<uint32_t>((r + 1) * seg + (s + 1) % seg);
            mesh.indices.push_back(a);
            mesh.indices.push_back(c);
            mesh.indices.push_back(b);
            mesh.indices.push_back(b);
            mesh.indices.push_back(c);
            mesh.indices.push_back(d);
        }
    }
    mesh.Validate();
    return mesh;
}

// 把两个网格拼成一个（不焊接）：用来构造「断面有两个环」的非法输入。
MeshData MergeRaw(const MeshData& a, const MeshData& b) {
    MeshData out = a;
    const uint32_t base = static_cast<uint32_t>(a.positions.size());
    for (size_t i = 0; i < b.positions.size(); ++i) {
        out.positions.push_back(b.positions[i]);
        out.normals.push_back(b.normals[i]);
    }
    for (uint32_t idx : b.indices) {
        out.indices.push_back(base + idx);
    }
    out.Validate();
    return out;
}

// 断面环的「圆度」：所有顶点到环心的距离（用 x/z 平面）。
struct RingShape {
    float radius_min = 0.0f;
    float radius_max = 0.0f;
    float cx = 0.0f;
    float cz = 0.0f;
};

RingShape MeasureLoop(const MeshData& mesh,
                      const jpov::model_editor::SeamLoop& loop) {
    RingShape shape;
    float sx = 0.0f;
    float sz = 0.0f;
    for (uint32_t v : loop.verts) {
        sx += mesh.positions[v].x();
        sz += mesh.positions[v].z();
    }
    shape.cx = sx / loop.verts.size();
    shape.cz = sz / loop.verts.size();
    shape.radius_min = 1e9f;
    shape.radius_max = 0.0f;
    for (uint32_t v : loop.verts) {
        const float d = std::hypot(mesh.positions[v].x() - shape.cx,
                                   mesh.positions[v].z() - shape.cz);
        shape.radius_min = std::min(shape.radius_min, d);
        shape.radius_max = std::max(shape.radius_max, d);
    }
    return shape;
}

struct Fixture {
    MeshData target;     // 细管 r=0.03，y ∈ [0,2]
    MeshData reference;  // 粗管 r=0.05，y ∈ [-2,2]，中心偏 x=+0.01
    float plane = 1.0f;

    Fixture()
        : target(MakeTube(0.03f, 0.0f, 2.0f, 24, 8)),
          reference(MakeTube(0.05f, -2.0f, 2.0f, 32, 8, 0.01f, 0.0f)) {}
};

}  // namespace

// 三种算法：都能成功，断面都贴合 reference，且不翻面。
TEST(MeshSeamTest, ThreeAlgorithmsAllConverge) {
    const Fixture fx;
    for (SeamAlgo algo : {SeamAlgo::kHarmonic, SeamAlgo::kArap, SeamAlgo::kTps}) {
        SeamOptions opt;
        opt.plane = fx.plane;
        opt.algo = algo;
        MeshData out;
        SeamStats st;
        std::string err;
        ASSERT_TRUE(jpov::model_editor::StitchSeam(
            fx.target, fx.reference, opt, &out, &st, &err))
            << jpov::model_editor::SeamAlgoName(algo) << " 失败: " << err;
        EXPECT_TRUE(st.ok);
        EXPECT_EQ(st.flipped_triangles, 0u) << jpov::model_editor::SeamAlgoName(algo);
        EXPECT_GT(st.band_vertices, 0u);
        // 断面必须贴合 reference 的断面（环重建后应≈0）。
        EXPECT_LT(st.loop_hausdorff_m, 2e-4f)
            << jpov::model_editor::SeamAlgoName(algo)
            << " hausdorff=" << st.loop_hausdorff_m;
        // 缝合后的断面半径必须真的变成 reference 的 0.05（不是恒真：形变前是 0.03）。
        jpov::model_editor::SeamLoop final_loop;
        std::string loop_err;
        ASSERT_TRUE(jpov::model_editor::seam_internal::ExtractCutLoop(
            out, 1, fx.plane, 1e-5f, &final_loop, &loop_err))
            << jpov::model_editor::SeamAlgoName(algo) << ": " << loop_err;
        const RingShape shape = MeasureLoop(out, final_loop);
        EXPECT_NEAR(shape.radius_max, 0.05f, 1e-3f)
            << jpov::model_editor::SeamAlgoName(algo);
        EXPECT_NEAR(shape.cx, 0.01f, 1e-3f) << jpov::model_editor::SeamAlgoName(algo);
    }
}

// 环重建：输出断面的顶点数 == reference 环顶点数（32），且逐点重合 ⇒ 可 1:1 焊接。
TEST(MeshSeamTest, RebuiltLoopMatchesReferenceVertexForVertex) {
    const Fixture fx;
    SeamOptions opt;
    opt.plane = fx.plane;
    opt.algo = SeamAlgo::kHarmonic;
    MeshData out;
    SeamStats st;
    std::string err;
    ASSERT_TRUE(jpov::model_editor::StitchSeam(
        fx.target, fx.reference, opt, &out, &st, &err))
        << err;
    ASSERT_TRUE(st.loop_rebuilt) << st.message;
    EXPECT_EQ(st.ref_loop_vertices, 32u);
    EXPECT_EQ(st.target_loop_vertices, 24u);  // 形变前
    EXPECT_GT(st.inserted_vertices, 0u);

    // 输出断面的顶点集合必须与 reference 断面的顶点集合一一对应。
    jpov::model_editor::SeamLoop out_loop;
    jpov::model_editor::SeamLoop ref_loop;
    std::string loop_err;
    ASSERT_TRUE(jpov::model_editor::seam_internal::ExtractCutLoop(
        out, 1, fx.plane, 1e-5f, &out_loop, &loop_err))
        << loop_err;
    // reference 要先按同一个平面「保下」切一刀才有断面环（ExtractCutLoop 只看切出来的边）。
    jpov::MeshData ref_cut;
    jpov::model_editor::ClipStats ref_clip;
    ASSERT_TRUE(jpov::model_editor::ClipMeshByAxis(
        fx.reference, 1, fx.plane, jpov::model_editor::ClipKeepSide::kLess, &ref_cut,
        &ref_clip));
    ASSERT_TRUE(jpov::model_editor::seam_internal::ExtractCutLoop(
        ref_cut, 1, fx.plane, 1e-5f, &ref_loop, &loop_err))
        << loop_err;
    ASSERT_EQ(out_loop.verts.size(), ref_loop.verts.size());
    // 逐点最近距离（无序集合比较）：必须为 0（同一批坐标）。
    float worst = 0.0f;
    for (uint32_t v : out_loop.verts) {
        float best = 1e9f;
        for (uint32_t rv : ref_loop.verts) {
            best = std::min(best, (out.positions[v] - fx.reference.positions[rv]).Norm());
        }
        worst = std::max(worst, best);
    }
    EXPECT_LT(worst, 1e-6f);
}

// 影响带外（远离断面）的顶点必须完全不动。
TEST(MeshSeamTest, FarVerticesStayPut) {
    const Fixture fx;
    SeamOptions opt;
    opt.plane = fx.plane;
    opt.algo = SeamAlgo::kArap;
    MeshData out;
    SeamStats st;
    std::string err;
    ASSERT_TRUE(jpov::model_editor::StitchSeam(
        fx.target, fx.reference, opt, &out, &st, &err))
        << err;
    // 形变前 24*9 个顶点；环重建会压缩掉一圈，所以按位置找顶部两圈。
    // 顶部 y=2.0 的顶点（最后一个 ring）在两种情况下都必须保持 y=2.0 / r=0.03。
    size_t checked = 0;
    for (size_t i = 0; i < out.positions.size(); ++i) {
        if (std::fabs(out.positions[i].y() - 2.0f) < 1e-6f) {
            const float r = std::hypot(out.positions[i].x(), out.positions[i].z());
            EXPECT_NEAR(r, 0.03f, 1e-5f);
            ++checked;
        }
    }
    EXPECT_EQ(checked, 24u);
}

// 非环断面（两根并排的管 ⇒ 两个环）必须被拒绝，且报错要指向「不是单环」。
TEST(MeshSeamTest, RejectsMultiLoopCrossSection) {
    Fixture fx;
    // 把 target 换成两根并排的细管：同一平面切出两个环。
    const MeshData two =
        MergeRaw(MakeTube(0.03f, 0.0f, 2.0f, 16, 4, 0.0f, 0.0f),
                 MakeTube(0.03f, 0.0f, 2.0f, 16, 4, 0.5f, 0.0f));
    SeamOptions opt;
    opt.plane = fx.plane;
    MeshData out;
    SeamStats st;
    std::string err;
    EXPECT_FALSE(jpov::model_editor::StitchSeam(
        two, fx.reference, opt, &out, &st, &err));
    EXPECT_NE(err.find("target"), std::string::npos) << err;
    EXPECT_NE(err.find("环"), std::string::npos) << err;
}

// 平面在模型上方 ⇒ 两个模型都切空 ⇒ 拒绝并给出原因。
TEST(MeshSeamTest, RejectsEmptyCut) {
    const Fixture fx;
    SeamOptions opt;
    opt.plane = 5.0f;
    MeshData out;
    SeamStats st;
    std::string err;
    EXPECT_FALSE(jpov::model_editor::StitchSeam(
        fx.target, fx.reference, opt, &out, &st, &err));
    EXPECT_NE(err.find("切空"), std::string::npos) << err;
}

// 旋转偏移：转 180° 后断面仍贴合（说明偏移滑条真的在改对应关系，不是空转）。
TEST(MeshSeamTest, OffsetRotatesCorrespondence) {
    Fixture fx;
    // 参考环做成椭圆（x 半径 0.06 / z 半径 0.03），这样不同对应角会得到不同形状。
    MeshData ref;
    ref.flags = static_cast<MeshVertexFlags>(
        static_cast<uint8_t>(MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(MeshVertexFlags::kNormal));
    const int seg = 32;
    const int rings = 4;
    for (int r = 0; r <= rings; ++r) {
        const float y = -2.0f + (r / static_cast<float>(rings)) * 4.0f;  // -2..2
        for (int s = 0; s < seg; ++s) {
            const float a = 2.0f * kPi * s / seg;
            ref.positions.push_back(Vec3f(0.06f * std::cos(a), y, 0.03f * std::sin(a)));
            ref.normals.push_back(Vec3f(std::cos(a), 0.0f, std::sin(a)));
        }
    }
    for (int r = 0; r < rings; ++r) {
        for (int s = 0; s < seg; ++s) {
            const uint32_t a = static_cast<uint32_t>(r * seg + s);
            const uint32_t b = static_cast<uint32_t>(r * seg + (s + 1) % seg);
            const uint32_t c = static_cast<uint32_t>((r + 1) * seg + s);
            const uint32_t d = static_cast<uint32_t>((r + 1) * seg + (s + 1) % seg);
            ref.indices.insert(ref.indices.end(), {a, c, b, b, c, d});
        }
    }
    ref.Validate();

    auto stitch_with_offset = [&](float deg, MeshData* o, SeamStats* s) {
        SeamOptions opt;
        opt.plane = fx.plane;
        opt.offset_deg = deg;
        std::string err;
        return jpov::model_editor::StitchSeam(fx.target, ref, opt, o,
                                                             s, &err);
    };
    MeshData a;
    MeshData b;
    SeamStats sa;
    SeamStats sb;
    ASSERT_TRUE(stitch_with_offset(0.0f, &a, &sa));
    ASSERT_TRUE(stitch_with_offset(180.0f, &b, &sb));
    // 两者都贴合断面，但形变后的几何必须不同（否则偏移是空操作）。
    EXPECT_LT(sa.loop_hausdorff_m, 2e-4f);
    EXPECT_LT(sb.loop_hausdorff_m, 2e-4f);
    float diff = 0.0f;
    const size_t n = std::min(a.positions.size(), b.positions.size());
    for (size_t i = 0; i < n; ++i) {
        diff = std::max(diff, (a.positions[i] - b.positions[i]).Norm());
    }
    EXPECT_GT(diff, 1e-4f) << "180° 偏移没有改变形变结果（对应关系没被改）";
}
