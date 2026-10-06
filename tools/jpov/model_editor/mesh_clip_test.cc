// JPOV 模型编辑器 — 沿 y 平面裁剪 单测（含真资材 yoga_pants 的 CPU 端到端）
//
// 覆盖：
//   - 全保留 / 全删除 / 跨界（1 顶点留、2 顶点留）三类；
//   - 边界顶点属性插值：位置 / UV / 法线（归一）/ 骨权（合并 + top-4 + 归一）；
//   - 绕序保持（面的几何法线方向不因裁剪翻转）；
//   - 索引化 + 共享边去重；
//   - 裁剪为空 → 返回 false（上层「报错并不做」的依据）；
//   - 端到端：真 yoga_pants.glb 裁剪 → WriteGlb → 生产 loader 读回自证。

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include "tools/common/utils.h"
#include "tools/jpov/interface/mesh.h"
#include "tools/jpov/model_editor/mesh_clip.h"
#include "tools/jpov/src/gltf_loader.h"
#include "tools/jpov/src/gltf_saver.h"

namespace jpov {
namespace model_editor {
namespace {

using jpov::MeshData;
using jpov::MeshVertexFlags;
using jpov::Vec2f;
using jpov::Vec3f;

MeshVertexFlags AllFlags() {
    return static_cast<MeshVertexFlags>(
        static_cast<uint8_t>(MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(MeshVertexFlags::kNormal) |
        static_cast<uint8_t>(MeshVertexFlags::kUV) |
        static_cast<uint8_t>(MeshVertexFlags::kJoints));
}

// 单个三角形：顶点 a/b/c（其余属性按顶点序填简单可辨认的值）。
MeshData MakeTriangle(const Vec3f& a, const Vec3f& b, const Vec3f& c) {
    MeshData m;
    m.flags = AllFlags();
    m.positions = {a, b, c};
    m.normals = {Vec3f(0, 0, 1), Vec3f(0, 0, 1), Vec3f(0, 0, 1)};
    m.uvs = {Vec2f(0, 0), Vec2f(1, 0), Vec2f(0, 1)};
    m.joint_indices = {{{0, 0, 0, 0}}, {{1, 0, 0, 0}}, {{2, 0, 0, 0}}};
    m.joint_weights = {{{1, 0, 0, 0}}, {{1, 0, 0, 0}}, {{1, 0, 0, 0}}};
    m.indices = {0, 1, 2};
    m.Validate();
    return m;
}

float Sum4(const std::array<float, 4>& w) {
    return w[0] + w[1] + w[2] + w[3];
}

// 三角形几何法线的 z 分量的符号（用于验证绕序未翻转）。
float NormalZ(const MeshData& m) {
    const Vec3f& a = m.positions[m.indices[0]];
    const Vec3f& b = m.positions[m.indices[1]];
    const Vec3f& c = m.positions[m.indices[2]];
    const float ux = b.x() - a.x(), uy = b.y() - a.y();
    const float vx = c.x() - a.x(), vy = c.y() - a.y();
    return ux * vy - uy * vx;  // 2*面积（带符号）
}

}  // namespace

TEST(MeshClip, AllAboveKeptUnchanged) {
    // 整个三角形都在 y=0.5 之上；keep above → 原样保留。
    const MeshData in = MakeTriangle({0, 1, 0}, {1, 1, 0}, {0, 2, 0});
    MeshData out;
    ClipStats st;
    ASSERT_TRUE(ClipMeshByY(in, 0.5f, ClipKeepSide::kGreater, &out, &st));
    EXPECT_EQ(st.input_triangles, 1u);
    EXPECT_EQ(st.output_triangles, 1u);
    EXPECT_EQ(st.new_boundary_vertices, 0u);
    EXPECT_EQ(st.kept_original_vertices, 3u);
    ASSERT_EQ(out.positions.size(), 3u);
    EXPECT_EQ(out.indices.size(), 3u);
}

TEST(MeshClip, AllBelowDroppedReturnsFalse) {
    const MeshData in = MakeTriangle({0, 1, 0}, {1, 1, 0}, {0, 2, 0});
    MeshData out;
    ClipStats st;
    // keep below（保留 y<=0.5）→ 三角形全在删除侧 → 空。
    EXPECT_FALSE(ClipMeshByY(in, 0.5f, ClipKeepSide::kLess, &out, &st));
    EXPECT_EQ(st.output_triangles, 0u);
}

TEST(MeshClip, OneVertexKept) {
    // (0,0,0)/(1,0,0) 在下，(0,1,0) 在上；keep above → 1 个三角形 + 2 个边界顶点。
    const MeshData in = MakeTriangle({0, 0, 0}, {1, 0, 0}, {0, 1, 0});
    MeshData out;
    ClipStats st;
    ASSERT_TRUE(ClipMeshByY(in, 0.5f, ClipKeepSide::kGreater, &out, &st));
    EXPECT_EQ(st.output_triangles, 1u);
    EXPECT_EQ(st.new_boundary_vertices, 2u);
    ASSERT_EQ(out.positions.size(), 3u);  // 1 原顶点 + 2 边界顶点
    // 所有 y >= 0.5 - eps。
    for (const Vec3f& p : out.positions) {
        EXPECT_GE(p.y(), 0.5f - 1e-5f);
    }
    // 边界顶点恰好落在 y=0.5。
    int on_plane = 0;
    for (const Vec3f& p : out.positions) {
        if (std::abs(p.y() - 0.5f) < 1e-5f) {
            ++on_plane;
        }
    }
    EXPECT_EQ(on_plane, 2);
}

TEST(MeshClip, TwoVerticesKeptGivesQuad) {
    // keep below（保留 y<=0.5）：2 个顶点在下 → 保留成 quad → 2 个三角形。
    const MeshData in = MakeTriangle({0, 0, 0}, {1, 0, 0}, {0, 1, 0});
    MeshData out;
    ClipStats st;
    ASSERT_TRUE(ClipMeshByY(in, 0.5f, ClipKeepSide::kLess, &out, &st));
    EXPECT_EQ(st.output_triangles, 2u);
    EXPECT_EQ(st.new_boundary_vertices, 2u);
    for (const Vec3f& p : out.positions) {
        EXPECT_LE(p.y(), 0.5f + 1e-5f);
    }
}

TEST(MeshClip, BoundaryAttributesInterpolate) {
    // 自定义 UV：uv(x,y) = (10+x, 20+y)，使 UV 随位置线性，便于核对插值。
    MeshData in;
    in.flags = AllFlags();
    in.positions = {{0, 0, 0}, {0, 1, 0}, {1, 0, 0}};
    in.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    in.uvs = {{10, 20}, {10, 21}, {11, 20}};
    in.joint_indices = {{{0, 0, 0, 0}}, {{0, 0, 0, 0}}, {{0, 0, 0, 0}}};
    in.joint_weights = {{{1, 0, 0, 0}}, {{1, 0, 0, 0}}, {{1, 0, 0, 0}}};
    in.indices = {0, 1, 2};
    in.Validate();

    MeshData out;
    ClipStats st;
    ASSERT_TRUE(ClipMeshByY(in, 0.5f, ClipKeepSide::kGreater, &out, &st));
    // 边 A(0,0,0)-B(0,1,0) 的交点 y=0.5 → uv 应插值到 (10, 20.5)。
    bool found = false;
    for (size_t i = 0; i < out.positions.size(); ++i) {
        const Vec3f& p = out.positions[i];
        if (std::abs(p.y() - 0.5f) < 1e-5f && std::abs(p.x()) < 1e-5f &&
            std::abs(p.z()) < 1e-5f) {
            found = true;
            EXPECT_NEAR(out.uvs[i].x(), 10.0f, 1e-5f);
            EXPECT_NEAR(out.uvs[i].y(), 20.5f, 1e-5f);
        }
    }
    EXPECT_TRUE(found);
    // 所有法线单位长。
    for (const Vec3f& n : out.normals) {
        const float len = std::sqrt(n.x() * n.x() + n.y() * n.y() + n.z() * n.z());
        EXPECT_NEAR(len, 1.0f, 1e-5f);
    }
}

TEST(MeshClip, BoundaryWeightsNormalized) {
    // A 权重全给 joint0，B 权重全给 joint1 → 交点应是 joint0/1 的混合且权重和为 1。
    MeshData in;
    in.flags = AllFlags();
    in.positions = {{0, 0, 0}, {0, 1, 0}, {1, 0, 0}};
    in.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    in.uvs = {{0, 0}, {0, 1}, {1, 0}};
    in.joint_indices = {{{0, 0, 0, 0}}, {{1, 0, 0, 0}}, {{1, 0, 0, 0}}};
    in.joint_weights = {{{1, 0, 0, 0}}, {{1, 0, 0, 0}}, {{1, 0, 0, 0}}};
    in.indices = {0, 1, 2};
    in.Validate();

    MeshData out;
    ClipStats st;
    ASSERT_TRUE(ClipMeshByY(in, 0.5f, ClipKeepSide::kGreater, &out, &st));
    // 边 A(0)-B(1) 的交点：t=0.5 → joint0 0.5 + joint1 0.5。
    bool checked = false;
    for (size_t i = 0; i < out.positions.size(); ++i) {
        const Vec3f& p = out.positions[i];
        if (std::abs(p.y() - 0.5f) < 1e-5f && std::abs(p.x()) < 1e-5f) {
            checked = true;
            EXPECT_NEAR(Sum4(out.joint_weights[i]), 1.0f, 1e-5f);
            // joint0 与 joint1 各 0.5（顺序可能因 top-4 排序不同，按 joint 命中检查）。
            float w0 = 0.0f;
            float w1 = 0.0f;
            for (int k = 0; k < 4; ++k) {
                if (out.joint_indices[i][k] == 0) {
                    w0 += out.joint_weights[i][k];
                }
                if (out.joint_indices[i][k] == 1) {
                    w1 += out.joint_weights[i][k];
                }
            }
            EXPECT_NEAR(w0, 0.5f, 1e-5f);
            EXPECT_NEAR(w1, 0.5f, 1e-5f);
        }
    }
    EXPECT_TRUE(checked);
}

TEST(MeshClip, WindingPreserved) {
    const MeshData in = MakeTriangle({0, 0, 0}, {1, 0, 0}, {0, 1, 0});
    MeshData out;
    ClipStats st;
    ASSERT_TRUE(ClipMeshByY(in, 0.5f, ClipKeepSide::kGreater, &out, &st));
    // 原始三角 (0,0,0),(1,0,0),(0,1,0)：叉积 z = +1（正向）。
    EXPECT_GT(NormalZ(in), 0.0f);
    EXPECT_GT(NormalZ(out), 0.0f);  // 裁剪后绕序保持。
}

TEST(MeshClip, SharedEdgeWeldsToOneVertex) {
    // 两个三角形共享一条跨裁剪面的边；该边上的交点应被去重（只建一次）。
    //
    // 顶点：A=(0,0,0) 在下方；B/C/D 在上方。
    // tri1 = (A,C,B)、tri2 = (A,B,D) 共享边 A-B。
    // 跨裁剪面的唯一边 = A-B（共享）+ A-C + A-D = 3 个 → 去重后边界顶点 3 个；
    // 若不去重则 A-B 会被两个三角形各建一次 = 4 个。
    MeshData in;
    in.flags = static_cast<MeshVertexFlags>(
        static_cast<uint8_t>(MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(MeshVertexFlags::kNormal));
    in.positions = {{0, 0, 0}, {0, 1, 0}, {1, 1, 0}, {-1, 1, 0}};
    in.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    in.indices = {0, 2, 1, 0, 1, 3};
    in.Validate();

    MeshData out;
    ClipStats st;
    ASSERT_TRUE(ClipMeshByY(in, 0.5f, ClipKeepSide::kGreater, &out, &st));
    EXPECT_EQ(st.new_boundary_vertices, 3u);
    int on_plane = 0;
    for (const Vec3f& p : out.positions) {
        if (std::abs(p.y() - 0.5f) < 1e-5f) {
            ++on_plane;
        }
    }
    EXPECT_EQ(on_plane, 3);
}

TEST(MeshClip, AxisXAndZ) {
    // X 轴：keep greater（保留 x>=0.5）→ 边界顶点 x 恰在 0.5。
    {
        const MeshData in = MakeTriangle({0, 0, 0}, {1, 0, 0}, {0, 1, 0});
        MeshData out;
        ClipStats st;
        ASSERT_TRUE(ClipMeshByAxis(in, /*axis*/ 0, 0.5f, ClipKeepSide::kGreater,
                                   &out, &st));
        EXPECT_EQ(st.new_boundary_vertices, 2u);
        for (const Vec3f& p : out.positions) {
            EXPECT_GE(p.x(), 0.5f - 1e-5f);
        }
    }
    // Z 轴：keep less（保留 z<=0.5）→ 2 个顶点在内 → quad → 2 三角形。
    {
        const MeshData in = MakeTriangle({0, 0, 0}, {0, 0, 1}, {0, 1, 0});
        MeshData out;
        ClipStats st;
        ASSERT_TRUE(ClipMeshByAxis(in, /*axis*/ 2, 0.5f, ClipKeepSide::kLess, &out,
                                   &st));
        EXPECT_EQ(st.output_triangles, 2u);
        for (const Vec3f& p : out.positions) {
            EXPECT_LE(p.z(), 0.5f + 1e-5f);
        }
    }
    // 三轴一致：ClipMeshByY 等于 ClipMeshByAxis(axis=1)。
    {
        const MeshData in = MakeTriangle({0, 0, 0}, {1, 0, 0}, {0, 1, 0});
        MeshData oy;
        MeshData oa;
        ClipStats sy;
        ClipStats sa;
        ASSERT_TRUE(ClipMeshByY(in, 0.5f, ClipKeepSide::kGreater, &oy, &sy));
        ASSERT_TRUE(ClipMeshByAxis(in, 1, 0.5f, ClipKeepSide::kGreater, &oa, &sa));
        EXPECT_EQ(sy.output_triangles, sa.output_triangles);
        EXPECT_EQ(sy.new_boundary_vertices, sa.new_boundary_vertices);
    }
}

// ==================== 真资材端到端 ====================

namespace {

std::string AssetPath(const std::string& rel) {
    const char* srcdir = std::getenv("TEST_SRCDIR");
    if (srcdir != nullptr) {
        std::string p = srcdir;
        if (!p.empty() && p.back() != '/') {
            p.push_back('/');
        }
        return p + "__main__/" + rel;
    }
    return jpov::GetProjectRoot() + rel;
}

constexpr char kPantsRel[] =
    "tools/jpov/test/object3d/clothing/yoga_pants/yoga_pants.glb";

}  // namespace

TEST(MeshClipE2E, YogaPantsClipSaveReload) {
    const std::string pants = AssetPath(kPantsRel);

    // 1) 读入真资材（CPU）。
    std::vector<GltfMeshEntry> entries;
    struct Collector {
        std::vector<GltfMeshEntry>* out;
    } collector{&entries};
    const auto cb = [](const GltfMeshEntry* e, void* user) {
        static_cast<Collector*>(user)->out->push_back(*e);
    };
    ASSERT_TRUE(jpov::LoadGltfScene(pants, cb, &collector));
    ASSERT_FALSE(entries.empty());

    // 取第一个 primitive 的 y 中位数作为裁剪面，保证「裁到东西但不裁空」。
    const MeshData& m0 = entries[0].mesh;
    float ymin = m0.positions[0].y();
    float ymax = m0.positions[0].y();
    for (const Vec3f& p : m0.positions) {
        ymin = std::min(ymin, p.y());
        ymax = std::max(ymax, p.y());
    }
    const float y0 = (ymin + ymax) * 0.5f;

    // 2) 裁剪：保留 y >= y0（删裤腿下方）。
    std::vector<GltfSaveMesh> clipped;
    size_t in_tris = 0;
    size_t out_tris = 0;
    for (const GltfMeshEntry& e : entries) {
        MeshData out;
        ClipStats st;
        const bool ok = ClipMeshByY(e.mesh, y0, ClipKeepSide::kGreater, &out, &st);
        in_tris += st.input_triangles;
        if (!ok) {
            continue;
        }
        out_tris += st.output_triangles;
        clipped.push_back(GltfSaveMesh{std::move(out), e.material});
    }
    ASSERT_GT(in_tris, 0u);
    ASSERT_GT(out_tris, 0u);
    EXPECT_LT(out_tris, in_tris) << "裁剪应确实删掉了裤腿下方的一部分";
    // 全部输出顶点在保留侧（容许 eps）。
    for (const GltfSaveMesh& sm : clipped) {
        for (const Vec3f& p : sm.mesh.positions) {
            EXPECT_GE(p.y(), y0 - 1e-4f);
        }
    }

    // 3) 写出 glb。
    const std::string out_path =
        "/tmp/jpov_clip_e2e_" + std::to_string(static_cast<long>(::getpid())) + ".glb";
    GltfSaveAsset asset;
    asset.name = "yoga_pants_clipped";
    asset.meshes = clipped;
    ASSERT_TRUE(jpov::WriteGlb(asset, out_path)) << "写 glb 失败: " << out_path;

    // 4) 生产 loader 读回自证：三角形数与几何位置一致。
    std::vector<GltfMeshEntry> reloaded;
    struct Collector2 {
        std::vector<GltfMeshEntry>* out;
    } collector2{&reloaded};
    const auto cb2 = [](const GltfMeshEntry* e, void* user) {
        static_cast<Collector2*>(user)->out->push_back(*e);
    };
    ASSERT_TRUE(jpov::LoadGltfScene(out_path, cb2, &collector2));
    size_t reload_tris = 0;
    for (const GltfMeshEntry& e : reloaded) {
        reload_tris += e.mesh.indices.empty() ? e.mesh.positions.size() / 3
                                              : e.mesh.indices.size() / 3;
    }
    EXPECT_EQ(reload_tris, out_tris) << "读回三角形数与写出不一致";
    // 读回后所有顶点仍在保留侧。
    for (const GltfMeshEntry& e : reloaded) {
        for (const Vec3f& p : e.mesh.positions) {
            EXPECT_GE(p.y(), y0 - 1e-4f);
        }
    }

    ::remove(out_path.c_str());
}

}  // namespace model_editor
}  // namespace jpov
