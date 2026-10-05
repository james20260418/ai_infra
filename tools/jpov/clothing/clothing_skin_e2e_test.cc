// JPOV 穿衣工具 — 软布自动蒙皮 端到端（CPU-only）自证
//
// 用**真实资产**跑完整 CPU 链路并自证：
//   1) 读 mixamo_male.glb（带蒙皮人体）→ 建三角形 + 蒙皮表 + 最近邻匹配器；
//   2) 读 harness_vest.glb（无骨衣服）→ 自动蒙皮（weight transfer）；
//   3) 写带 skin 的 glb（GltfSaveAsset.skin = 人体骨架）；
//   4) 用**生产 loader** 读回：JOINTS_0/WEIGHTS_0 存在、数组对齐、权重归一、骨数一致。
//
// 这是「工具产出的蒙皮 glb 真能被打开且权重自洽」的直接证据（不跑 GL）。
// ⚠️ 本测试对**原始（未仿真贴合）**的背心做蒙皮，只作链路自证；贴身对齐 + 验收靠交互工具。

#include <array>
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "geom/3d/triangle_matcher_3d.h"
#include "tools/common/utils.h"
#include "tools/jpov/clothing/weight_transfer.h"
#include "tools/jpov/interface/mesh.h"
#include "tools/jpov/interface/skeleton_types.h"
#include "tools/jpov/src/gltf_loader.h"
#include "tools/jpov/src/gltf_saver.h"

namespace jpov {
namespace clothing {
namespace {

// 定位真资材（与其它 gold/loader test 同约定：优先 TEST_SRCDIR）。
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

constexpr char kBodyRel[] = "tools/jpov/test/object3d/mixamo_male/mixamo_male.glb";
constexpr char kVestRel[] = "tools/jpov/test/object3d/clothing/harness_vest/harness_vest.glb";

// 收集身体几何的三角形 + 蒙皮角表（回调式，与 clothing_init 同构）。
struct BodyCollector {
    std::vector<Triangle3d> tris;
    BodySkinTable table;
};

void CollectBody(const GltfMeshEntry* entry, void* user_data) {
    CHECK(entry != nullptr);
    CHECK(user_data != nullptr);
    BodyCollector* c = static_cast<BodyCollector*>(user_data);
    AppendMeshTriangles(entry->mesh, &c->tris, &c->table);
}

// 权重和（4 组）。
float Sum4(const std::array<float, 4>& w) {
    return w[0] + w[1] + w[2] + w[3];
}

}  // namespace

TEST(ClothingSkinE2E, TransferThenSaveThenReload) {
    const std::string body_path = AssetPath(kBodyRel);
    const std::string vest_path = AssetPath(kVestRel);

    // 1) 身体：三角形 + 蒙皮表 + 匹配器。
    BodyCollector bc;
    ASSERT_TRUE(jpov::LoadGltfScene(body_path, &CollectBody, &bc));
    ASSERT_FALSE(bc.tris.empty());
    ASSERT_EQ(bc.table.triangle_count(), bc.tris.size())
        << "人体蒙皮表与三角形应同序同长";
    geom::TriangleMatcher3d<double> matcher(/*local_distance*/0.05, /*grid*/0.01,
                                            std::move(bc.tris));

    // 2) 衣服（第一 primitive）+ 自动蒙皮。
    MeshData vest;
    GltfMaterialInfo vest_mat;
    ASSERT_TRUE(jpov::LoadGltf(vest_path, &vest, &vest_mat));
    ASSERT_FALSE(vest.positions.empty());
    const size_t vcount = vest.positions.size();

    const SkinTransferStats st = TransferSkinWeights(
        bc.table, matcher, &vest, /*seed_eps*/0.01f, /*weld*/kWeldToleranceM, /*max_inf*/4,
        /*growth*/8);
    EXPECT_EQ(st.vertex_count, vcount);
    EXPECT_EQ(st.seed_vertex_count + st.non_seed_vertex_count, vcount);
    EXPECT_GE(st.growth_passes, 1u);
    // 权重归一 + ≤4 影响。
    for (size_t v = 0; v < vcount; ++v) {
        EXPECT_NEAR(Sum4(vest.joint_weights[v]), 1.0f, 1e-4f) << "v=" << v;
        int nonzero = 0;
        for (int k = 0; k < 4; ++k) {
            if (vest.joint_weights[v][k] > 0.0f) {
                ++nonzero;
            }
        }
        EXPECT_LE(nonzero, 4);
    }
    EXPECT_TRUE(MeshHasFlag(vest.flags, MeshVertexFlags::kJoints));

    // 缝合一致性（开裂回归）：位置重合的重复顶点必须拿到**完全相同**的权重，否则蒙皮后
    // 缝合两侧分离（"开裂"）。harness_vest 有上万这类重复顶点（导出器在 UV/材质缝合处拆开）。
    {
        const double kQuant = 1.0e4;  // 量化到 0.1mm，与 kWeldToleranceM 同量级
        std::map<std::array<long long, 3>, size_t> first_of_cell;
        size_t duplicate_vertex_count = 0;
        for (size_t v = 0; v < vcount; ++v) {
            const std::array<long long, 3> q = {
                std::llround(static_cast<double>(vest.positions[v].x()) * kQuant),
                std::llround(static_cast<double>(vest.positions[v].y()) * kQuant),
                std::llround(static_cast<double>(vest.positions[v].z()) * kQuant)};
            const auto it = first_of_cell.find(q);
            if (it == first_of_cell.end()) {
                first_of_cell.emplace(q, v);
                continue;
            }
            ++duplicate_vertex_count;
            const size_t ref = it->second;
            EXPECT_EQ(vest.joint_indices[v], vest.joint_indices[ref]) << "v=" << v;
            for (int k = 0; k < 4; ++k) {
                EXPECT_NEAR(vest.joint_weights[v][k], vest.joint_weights[ref][k], 1e-6f)
                    << "v=" << v << " k=" << k;
            }
        }
        // 资产确实含缝合重复顶点（否则该断言空转、无意义）。
        EXPECT_GT(duplicate_vertex_count, 0u);
    }

    // 3) 写带 skin 的 glb。
    std::vector<SkeletonType> skins;
    ASSERT_TRUE(jpov::LoadGltfSkeleton(body_path, &skins));
    ASSERT_FALSE(skins.empty());
    const int bone_count = skins[0].bone_count();

    GltfSaveAsset asset;
    asset.name = "vest_skinned";
    GltfSaveMesh sm;
    sm.mesh = vest;
    sm.material = vest_mat;
    asset.meshes.push_back(std::move(sm));
    asset.skin = skins[0];

    const std::string out = "/tmp/jpov_clothing_skin_e2e.glb";
    ASSERT_TRUE(jpov::WriteGlb(asset, out));

    // 4) 读回：骨骼通道在、数组对齐、权重归一、骨数一致。
    MeshData back;
    GltfMaterialInfo back_mat;
    ASSERT_TRUE(jpov::LoadGltf(out, &back, &back_mat));
    EXPECT_TRUE(MeshHasFlag(back.flags, MeshVertexFlags::kJoints));
    EXPECT_EQ(back.joint_indices.size(), back.positions.size());
    EXPECT_EQ(back.joint_weights.size(), back.positions.size());
    for (size_t v = 0; v < back.positions.size(); ++v) {
        EXPECT_NEAR(Sum4(back.joint_weights[v]), 1.0f, 1e-3f) << "reloaded v=" << v;
    }
    std::vector<SkeletonType> back_skins;
    ASSERT_TRUE(jpov::LoadGltfSkeleton(out, &back_skins));
    ASSERT_FALSE(back_skins.empty());
    EXPECT_EQ(back_skins[0].bone_count(), bone_count);
}

}  // namespace clothing
}  // namespace jpov
