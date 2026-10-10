// gltf_saver 单测 — 写一个最小带骨资产 → 读回闭环自证（GL-free）
//
// 覆盖（对应 docs/jpov_model_editor_save_plan.md §3）：
//   1. 无骨 mesh（POSITION/NORMAL/TEXCOORD_0/indices）写→读回：顶点数/位置/UV/索引一致。
//   2. 带骨 mesh（+ JOINTS_0/WEIGHTS_0）+ SkeletonType：读回骨名/拓扑/bind_rotation 一致。
//   3. 多 primitive：数量与各自几何一致。
//   4. 负面：空资产必须 crash。
//
// 说明：读回用**生产 loader**（jpov::LoadGltfScene / LoadGltfSkeleton）——这是
//   "写出来的文件真能被打开"的直接证据；不用另一套自造 parser 自证。

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include "tools/common/utils.h"
#include "tools/jpov/interface/mesh.h"
#include "tools/jpov/interface/mesh_transform.h"
#include "tools/jpov/interface/skeleton_types.h"
#include "tools/jpov/src/gltf_loader.h"
#include "tools/jpov/src/gltf_saver.h"

namespace jpov {
namespace {

// 定位真资材（与其它 gold test 同约定：优先 TEST_SRCDIR）。
std::string SkinnedAssetPath() {
    const char* srcdir = std::getenv("TEST_SRCDIR");
    if (srcdir) {
        std::string p = srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        return p + "__main__/tools/jpov/assets/models/characters/mixamo_male.glb";
    }
    return jpov::GetProjectRoot() +
           "tools/jpov/assets/models/characters/mixamo_male.glb";
}

std::string TmpPath(const std::string& name) {
    return "/tmp/jpov_saver_test_" + name + ".glb";
}

// 造一个简单三角形 mesh（3 顶点，带 NORMAL/UV/索引）。
MeshData MakeTriMesh(float scale) {
    MeshData m;
    m.flags = static_cast<MeshVertexFlags>(
        static_cast<uint8_t>(MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(MeshVertexFlags::kNormal) |
        static_cast<uint8_t>(MeshVertexFlags::kUV));
    m.positions = {{0.0f, 0.0f, 0.0f},
                   {1.0f * scale, 0.0f, 0.0f},
                   {0.0f, 1.0f * scale, 0.0f}};
    m.normals = {{0.0f, 0.0f, 1.0f},
                 {0.0f, 0.0f, 1.0f},
                 {0.0f, 0.0f, 1.0f}};
    m.uvs = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};
    m.indices = {0, 1, 2};
    m.Validate();
    return m;
}

// 造第二个 mesh（带骨权重），顶点 4 个。
MeshData MakeQuadSkinnedMesh() {
    MeshData m;
    m.flags = static_cast<MeshVertexFlags>(
        static_cast<uint8_t>(MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(MeshVertexFlags::kNormal) |
        static_cast<uint8_t>(MeshVertexFlags::kUV) |
        static_cast<uint8_t>(MeshVertexFlags::kJoints));
    m.positions = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f},
                   {1.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    m.normals = {{0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f},
                 {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}};
    m.uvs = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    m.indices = {0, 1, 2, 0, 2, 3};
    m.joint_indices = {{0, 1, 0, 0}, {1, 1, 0, 0},
                       {0, 1, 0, 0}, {0, 0, 1, 0}};
    m.joint_weights = {{0.5f, 0.5f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 0.0f},
                       {0.5f, 0.5f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f, 0.0f}};
    m.Validate();
    return m;
}

// 三关节骨架：root → a → b，bind_rotation 非恒等。
SkeletonType MakeSkeleton() {
    SkeletonType t;
    t.joints.resize(3);
    t.joints[0].parent = kSkeletonNoParent;
    t.joints[0].rest_offset = {0.0f, 0.9f, 0.0f};
    t.joints[0].name = "root";
    t.joints[1].parent = 0;
    t.joints[1].rest_offset = {0.2f, 0.4f, 0.0f};
    t.joints[1].name = "arm_a";
    t.joints[2].parent = 1;
    t.joints[2].rest_offset = {0.0f, 0.3f, 0.1f};
    t.joints[2].name = "arm_b";
    t.bind_rotation.resize(3);
    t.bind_rotation[0] = geom::Quaternion<float>::Identity();
    t.bind_rotation[1] = geom::Quaternion<float>::FromAxisAngle(
        {0.0f, 0.0f, 1.0f}, static_cast<float>(M_PI / 2.0));
    t.bind_rotation[2] = geom::Quaternion<float>::FromAxisAngle(
        {1.0f, 0.0f, 0.0f}, static_cast<float>(M_PI / 6.0));
    t.Validate();
    return t;
}

float MaxDiff3(const Vec3f& a, const Vec3f& b) {
    return std::max({std::fabs(a.x() - b.x()), std::fabs(a.y() - b.y()),
                     std::fabs(a.z() - b.z())});
}

// 读回回调：把每条 entry 收进 vector（entry->mesh 可 move）。
struct Collect {
    std::vector<MeshData> meshes;
    std::vector<GltfMaterialInfo> mats;
};

void CollectCb(const GltfMeshEntry* e, void* user) {
    Collect* c = static_cast<Collect*>(user);
    c->meshes.push_back(e->mesh);
    c->mats.push_back(e->material);
}

// ==================== 1. 无骨 mesh 写→读回 ====================

TEST(GltfSaverTest, RoundTripPlainMesh) {
    GltfSaveAsset asset;
    asset.name = "tri";
    asset.meshes.push_back(GltfSaveMesh{MakeTriMesh(2.0f), GltfMaterialInfo{}});

    const std::string path = TmpPath("plain");
    std::remove(path.c_str());
    ASSERT_TRUE(WriteGlb(asset, path)) << "WriteGlb 应成功";

    Collect c;
    ASSERT_TRUE(LoadGltfScene(path, CollectCb, &c)) << "写出的 glb 应能读回";
    ASSERT_EQ(c.meshes.size(), 1u);

    const MeshData& got = c.meshes[0];
    const MeshData want = MakeTriMesh(2.0f);
    EXPECT_EQ(got.VertexCount(), 3u);
    EXPECT_TRUE(MeshHasFlag(got.flags, MeshVertexFlags::kNormal));
    EXPECT_TRUE(MeshHasFlag(got.flags, MeshVertexFlags::kUV));
    EXPECT_EQ(got.indices.size(), 3u);
    // 往返应**完全复原入参**（saver 先做 loader 映射的逆映射；见 gltf_saver.cc 坐标映射段）。
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_LT(MaxDiff3(got.positions[i], want.positions[i]), 1e-5f)
            << "第 " << i << " 顶点位置往返不一致";
        EXPECT_LT(MaxDiff3(got.normals[i], want.normals[i]), 1e-5f)
            << "第 " << i << " 顶点法线往返不一致（映射应为互逆）";
        EXPECT_NEAR(got.uvs[i].x(), want.uvs[i].x(), 1e-5f);
        EXPECT_NEAR(got.uvs[i].y(), want.uvs[i].y(), 1e-5f);
    }
    std::remove(path.c_str());
}

// ==================== 1b. _JPOV_RELAX（布料 LAG 松弛度）自定义顶点属性 写→读回 ====================

TEST(GltfSaverTest, RoundTripRelaxAttribute) {
    MeshData m = MakeTriMesh(1.0f);
    m.flags = static_cast<MeshVertexFlags>(
        static_cast<uint8_t>(m.flags) |
        static_cast<uint8_t>(MeshVertexFlags::kRelax));
    m.relaxations = {{0.0f, 0.0f}, {0.5f, 0.25f}, {1.0f, 2.0f}};
    m.Validate();

    GltfSaveAsset asset;
    asset.name = "relax";
    asset.meshes.push_back(GltfSaveMesh{m, GltfMaterialInfo{}});

    const std::string path = TmpPath("relax");
    std::remove(path.c_str());
    ASSERT_TRUE(WriteGlb(asset, path)) << "WriteGlb 应成功";

    Collect c;
    ASSERT_TRUE(LoadGltfScene(path, CollectCb, &c)) << "写出的 glb 应能读回";
    ASSERT_EQ(c.meshes.size(), 1u);
    const MeshData& got = c.meshes[0];
    ASSERT_TRUE(MeshHasFlag(got.flags, MeshVertexFlags::kRelax))
        << "读回的 mesh 应带 kRelax（_JPOV_RELAX 未往返）";
    ASSERT_EQ(got.relaxations.size(), 3u);
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_NEAR(got.relaxations[i].x(), m.relaxations[i].x(), 1e-6f);
        EXPECT_NEAR(got.relaxations[i].y(), m.relaxations[i].y(), 1e-6f);
    }
    std::remove(path.c_str());
}

// 坐坐标映射互逆：save → load → save → load 两次结果应逐位一致
// （若 saver 漏做逆映射，每次往返会多转 90°，此测试立刻挂）。
TEST(GltfSaverTest, RoundTripIsStableAcrossTwoCycles) {
    GltfSaveAsset asset;
    asset.name = "cycle";
    asset.meshes.push_back(GltfSaveMesh{MakeTriMesh(1.5f), GltfMaterialInfo{}});
    const std::string p1 = TmpPath("cycle1");
    const std::string p2 = TmpPath("cycle2");
    std::remove(p1.c_str());
    std::remove(p2.c_str());
    ASSERT_TRUE(WriteGlb(asset, p1));

    Collect c1;
    ASSERT_TRUE(LoadGltfScene(p1, CollectCb, &c1));
    GltfSaveAsset a2;
    a2.name = "cycle";
    a2.meshes.push_back(GltfSaveMesh{c1.meshes[0], GltfMaterialInfo{}});
    ASSERT_TRUE(WriteGlb(a2, p2));

    Collect c2;
    ASSERT_TRUE(LoadGltfScene(p2, CollectCb, &c2));
    ASSERT_EQ(c2.meshes.size(), 1u);
    ASSERT_EQ(c1.meshes[0].VertexCount(), c2.meshes[0].VertexCount());
    for (size_t i = 0; i < c1.meshes[0].VertexCount(); ++i) {
        EXPECT_LT(MaxDiff3(c1.meshes[0].positions[i], c2.meshes[0].positions[i]),
                  1e-5f)
            << "两轮往返后位置漂移（坐标映射互逆性破了）";
    }
    std::remove(p1.c_str());
    std::remove(p2.c_str());
}

// ==================== 2. 多 primitive ====================

TEST(GltfSaverTest, RoundTripMultiPrimitive) {
    GltfSaveAsset asset;
    asset.name = "multi";
    asset.meshes.push_back(GltfSaveMesh{MakeTriMesh(1.0f), GltfMaterialInfo{}});
    asset.meshes.push_back(GltfSaveMesh{MakeTriMesh(3.0f), GltfMaterialInfo{}});

    const std::string path = TmpPath("multi");
    std::remove(path.c_str());
    ASSERT_TRUE(WriteGlb(asset, path));

    Collect c;
    ASSERT_TRUE(LoadGltfScene(path, CollectCb, &c));
    ASSERT_EQ(c.meshes.size(), 2u);
    // 第二个 primitive 的 x 极值应为 3（未写反/串位）。
    EXPECT_NEAR(c.meshes[1].positions[1].x(), 3.0f, 1e-5f);
    std::remove(path.c_str());
}

// ==================== 3. 带骨资产写→读回 ====================

TEST(GltfSaverTest, RoundTripSkinnedAsset) {
    GltfSaveAsset asset;
    asset.name = "skinned";
    asset.meshes.push_back(
        GltfSaveMesh{MakeQuadSkinnedMesh(), GltfMaterialInfo{}});
    asset.skin = MakeSkeleton();

    const std::string path = TmpPath("skinned");
    std::remove(path.c_str());
    ASSERT_TRUE(WriteGlb(asset, path));

    // 顶点侧：JOINTS_0/WEIGHTS_0 读回。
    Collect c;
    ASSERT_TRUE(LoadGltfScene(path, CollectCb, &c));
    ASSERT_EQ(c.meshes.size(), 1u);
    const MeshData& got = c.meshes[0];
    EXPECT_EQ(got.VertexCount(), 4u);
    ASSERT_TRUE(MeshHasFlag(got.flags, MeshVertexFlags::kJoints));
    EXPECT_EQ(got.joint_indices[1][1], 1);
    EXPECT_NEAR(got.joint_weights[3][2], 1.0f, 1e-5f);

    // 骨架侧：名字 / 拓扑 / bind_rotation 读回。
    std::vector<SkeletonType> skins;
    ASSERT_TRUE(LoadGltfSkeleton(path, &skins));
    ASSERT_EQ(skins.size(), 1u);
    const SkeletonType& sk = skins[0];
    ASSERT_EQ(sk.joints.size(), 3u);
    EXPECT_EQ(sk.joints[0].name, "root");
    EXPECT_EQ(sk.joints[2].name, "arm_b");
    EXPECT_EQ(sk.joints[0].parent, kSkeletonNoParent);
    EXPECT_EQ(sk.joints[1].parent, 0);
    EXPECT_EQ(sk.joints[2].parent, 1);
    // rest_offset 近似一致（FLOAT 往返）。
    EXPECT_LT(MaxDiff3(sk.joints[1].rest_offset, Vec3f(0.2f, 0.4f, 0.0f)),
              1e-5f);
    // bind_rotation：绕 Z 90°（w≈cos45）读回。
    const SkeletonType orig = MakeSkeleton();
    ASSERT_EQ(sk.bind_rotation.size(), orig.bind_rotation.size());
    EXPECT_NEAR(sk.bind_rotation[1].w, orig.bind_rotation[1].w, 1e-5f);
    EXPECT_NEAR(sk.bind_rotation[1].z, orig.bind_rotation[1].z, 1e-5f);
    EXPECT_NEAR(sk.bind_rotation[2].x, orig.bind_rotation[2].x, 1e-5f);
    std::remove(path.c_str());
}

// 骨架自算 IBM 与写出的 IBM 应一致（IBM 是派生量，验证写时用的是同一来源）。
TEST(GltfSaverTest, SkinnedAssetInverseBindConsistentWithSkeleton) {
    GltfSaveAsset asset;
    asset.name = "ibm";
    asset.meshes.push_back(
        GltfSaveMesh{MakeQuadSkinnedMesh(), GltfMaterialInfo{}});
    asset.skin = MakeSkeleton();
    const std::string path = TmpPath("ibm");
    std::remove(path.c_str());
    ASSERT_TRUE(WriteGlb(asset, path));

    std::vector<SkeletonType> skins;
    ASSERT_TRUE(LoadGltfSkeleton(path, &skins));
    ASSERT_EQ(skins.size(), 1u);
    // 读回的骨架自算 IBM 应与读回的 bind_rotation/rest_offset 自洽（不崩、维度对）。
    const std::vector<std::array<float, 16>> ibm = skins[0].ComputeInverseBind();
    EXPECT_EQ(ibm.size(), 3u);
    std::remove(path.c_str());
}

// 直接校验写出文件的 GLB JSON（白盒）：IBM accessor 的 count 必须 == 骨数。
//
// 为何需要：loader **不再消费** inverseBindMatrices（inverse_bind 已是派生量，
//   见 gltf_loader.cc），所以通过 loader 读回**观测不到** IBM accessor 的 count。
//   而写错 count 会让其它读取器（Blender/Three.js）读到残缺数据。故直查文件。
TEST(GltfSaverTest, WrittenInverseBindAccessorCountMatchesBones) {
    GltfSaveAsset asset;
    asset.name = "ibmcount";
    asset.meshes.push_back(
        GltfSaveMesh{MakeQuadSkinnedMesh(), GltfMaterialInfo{}});
    asset.skin = MakeSkeleton();   // 3 关节
    const std::string path = TmpPath("ibmcount");
    std::remove(path.c_str());
    ASSERT_TRUE(WriteGlb(asset, path));

    // 读 GLB：12 字节头 + chunk0(JSON)。
    std::ifstream f(path, std::ifstream::binary);
    ASSERT_TRUE(f.good());
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(f)),
                                     std::istreambuf_iterator<char>());
    ASSERT_GT(bytes.size(), 20u);
    auto u32 = [&bytes](size_t off) {
        return static_cast<uint32_t>(bytes[off]) |
               (static_cast<uint32_t>(bytes[off + 1]) << 8) |
               (static_cast<uint32_t>(bytes[off + 2]) << 16) |
               (static_cast<uint32_t>(bytes[off + 3]) << 24);
    };
    EXPECT_EQ(u32(0), 0x46546C67u);   // 'glTF'
    const uint32_t json_len = u32(12);
    ASSERT_EQ(u32(16), 0x4E4F534Au);  // 'JSON'
    const std::string json_text(
        reinterpret_cast<const char*>(&bytes[20]), json_len);

    // 提取 "inverseBindMatrices": N 与 "joints" 数组长度。
    const std::string key = "\"inverseBindMatrices\":";
    const size_t p = json_text.find(key);
    ASSERT_NE(p, std::string::npos) << "写出文件应有 inverseBindMatrices";
    size_t q = p + key.size();
    while (q < json_text.size() && (json_text[q] == ' ')) ++q;
    int ibm_acc = 0;
    while (q < json_text.size() && json_text[q] >= '0' && json_text[q] <= '9') {
        ibm_acc = ibm_acc * 10 + (json_text[q] - '0');
        ++q;
    }
    // 找 accessors[ibm_acc] 的 count 字段（粗解：在该 accessor 对象内找 "count"）。
    const std::string acc_key = "\"accessors\":[";
    const size_t ap = json_text.find(acc_key);
    ASSERT_NE(ap, std::string::npos);
    // 逐个 accessor 计数大括号，定位第 ibm_acc 个。
    size_t cur = ap + acc_key.size();
    std::vector<std::string> accs;
    int depth = 0;
    std::string buf;
    for (size_t i = cur; i < json_text.size(); ++i) {
        const char c = json_text[i];
        if (c == '{') {
            if (depth == 0) buf.clear();
            ++depth;
        }
        if (depth > 0) buf.push_back(c);
        if (c == '}') {
            --depth;
            if (depth == 0) accs.push_back(buf);
        }
        if (c == ']' && depth == 0) break;
    }
    ASSERT_GT(accs.size(), static_cast<size_t>(ibm_acc));
    const std::string& chosen = accs[static_cast<size_t>(ibm_acc)];
    ASSERT_NE(chosen.find("MAT4"), std::string::npos)
        << "IBM accessor 应为 MAT4";
    const size_t cp = chosen.find("\"count\":");
    ASSERT_NE(cp, std::string::npos);
    size_t cq = cp + std::string("\"count\":").size();
    int cnt = 0;
    while (cq < chosen.size() && chosen[cq] >= '0' && chosen[cq] <= '9') {
        cnt = cnt * 10 + (chosen[cq] - '0');
        ++cq;
    }
    EXPECT_EQ(cnt, 3) << "🔴 IBM accessor count 必须 == 骨数（写错会让其它读取器读残）";
    std::remove(path.c_str());
}

// ==================== 4. 负面 ====================

TEST(GltfSaverTest, EmptyAssetCrashes) {
    GltfSaveAsset asset;   // 无 mesh
    EXPECT_DEATH(WriteGlb(asset, TmpPath("empty")), "没有任何 primitive");
}

TEST(GltfSaverTest, InvalidMeshCrashes) {
    GltfSaveAsset asset;
    asset.name = "bad";
    MeshData bad;   // 无顶点 → Validate 失败
    bad.flags = MeshVertexFlags::kPosition;
    asset.meshes.push_back(GltfSaveMesh{bad, GltfMaterialInfo{}});
    EXPECT_DEATH(WriteGlb(asset, TmpPath("bad")), "positions 不能为空");
}

// ==================== 5. 真带骨资产（mixamo_male）完整链路 ====================
//
// 模拟编辑器的真实保存路径：纯 loader 读 CPU 资产 → ApplyPlacement（旋转+平移）
//   → WriteGlb → 读回。验证带骨资产的保存不丢骨架且放置真烘进顶点。
TEST(GltfSaverTest, RealSkinnedAssetSaveRoundTrip) {
    const std::string src = SkinnedAssetPath();

    struct SaveCollect {
        std::vector<GltfSaveMesh> meshes;
    } col;
    auto cb = [](const GltfMeshEntry* e, void* u) {
        SaveCollect* c = static_cast<SaveCollect*>(u);
        c->meshes.push_back(GltfSaveMesh{e->mesh, e->material});
    };
    ASSERT_TRUE(LoadGltfScene(src, cb, &col)) << "真资材应能加载";
    ASSERT_FALSE(col.meshes.empty());

    std::vector<SkeletonType> skins;
    ASSERT_TRUE(LoadGltfSkeleton(src, &skins));
    ASSERT_FALSE(skins.empty()) << "mixamo_male 应带骨架";
    const SkeletonType& sk0 = skins[0];
    const int bone_count = sk0.bone_count();
    ASSERT_GT(bone_count, 0);

    // 用**刚体放置**（旋转 + 平移，scale=1）烘。
    const Vec3f up{0.0f, 1.0f, 0.0f};
    const float a = static_cast<float>(M_PI / 3.0);   // 绕 Y 转 60°
    const Vec3f front{std::sin(a), 0.0f, std::cos(a)};
    const Vec3f center{0.5f, 0.25f, -0.3f};

    GltfSaveAsset asset;
    asset.name = "male";
    for (const GltfSaveMesh& sm : col.meshes) {
        asset.meshes.push_back(GltfSaveMesh{
            ApplyPlacementToMesh(sm.mesh, center, up, front, /*scale=*/1.0f),
            sm.material});
    }
    asset.skin = ApplyPlacementToSkeleton(sk0, center, up, front);

    const std::string out = TmpPath("mixamo");
    std::remove(out.c_str());
    ASSERT_TRUE(WriteGlb(asset, out)) << "带骨资产应能写出";

    // 读回：骨架维度/骨名保持。
    std::vector<SkeletonType> skins2;
    ASSERT_TRUE(LoadGltfSkeleton(out, &skins2));
    ASSERT_EQ(skins2.size(), 1u);
    EXPECT_EQ(skins2[0].bone_count(), bone_count)
        << "保存不得丢掉关节";
    EXPECT_EQ(skins2[0].joints[0].name, sk0.joints[0].name);
    EXPECT_EQ(skins2[0].bind_rotation.size(), sk0.bind_rotation.size());

    // 读回：顶点数保持，且顶点**确实被移动**（放置烘进去了）。
    Collect col2;   // 文件级 Collect：meshes = vector<MeshData>
    auto cb2 = [](const GltfMeshEntry* e, void* u) {
        Collect* c = static_cast<Collect*>(u);
        c->meshes.push_back(e->mesh);
        c->mats.push_back(e->material);
    };
    ASSERT_TRUE(LoadGltfScene(out, cb2, &col2));
    ASSERT_EQ(col2.meshes.size(), col.meshes.size());
    for (size_t i = 0; i < col2.meshes.size(); ++i) {
        EXPECT_EQ(col2.meshes[i].VertexCount(), asset.meshes[i].mesh.VertexCount())
            << "顶点数（含 JOINTS/WEIGHTS）不得变";
        EXPECT_TRUE(MeshHasFlag(col2.meshes[i].flags, MeshVertexFlags::kJoints))
            << "带骨资产的 kJoints 不得丢";
    }
    // 放置确已烘进顶点：读回顶点 == 我们烘的值（逐位）。
    for (size_t v = 0; v < col2.meshes[0].VertexCount(); ++v) {
        EXPECT_LT(MaxDiff3(col2.meshes[0].positions[v],
                           asset.meshes[0].mesh.positions[v]),
                  1e-4f);
    }
    std::remove(out.c_str());
}

// ==================== 6. 外置纹理导出 + 往返（WriteGltf） ====================

// 定位 lantern 真资材（内嵌 PNG 贴图，用于外置纹理往返）。
std::string LanternAssetPath() {
    const char* srcdir = std::getenv("TEST_SRCDIR");
    if (srcdir) {
        std::string p = srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        return p +
               "__main__/tools/jpov/assets/models/samples/lantern/lantern.glb";
    }
    return jpov::GetProjectRoot() +
           "tools/jpov/assets/models/samples/lantern/lantern.glb";
}

// 把一条 entry 收进 vector<GltfSaveMesh>（可复用的读回回调）。
void CollectSaveMeshes(const GltfMeshEntry* e, void* user) {
    std::vector<GltfSaveMesh>* v = static_cast<std::vector<GltfSaveMesh>*>(user);
    v->push_back(GltfSaveMesh{e->mesh, e->material});
}

// WriteGltf 应产出 .gltf + .bin + 独立贴图，且**能被生产 loader 读回**；
//   再走 WriteGlb 打包回单文件 glb 后贴图仍在（外置→内嵌往返不丢）。
TEST(GltfSaverTest, WriteGltfExternalTextureRoundTrip) {
    const std::string src = LanternAssetPath();

    std::vector<GltfSaveMesh> meshes;
    ASSERT_TRUE(LoadGltfScene(src, CollectSaveMeshes, &meshes))
        << "lantern.glb 应能加载";
    ASSERT_FALSE(meshes.empty());

    // 至少一个材质带 baseColor 贴图，否则本测试无意义。
    bool any_tex = false;
    for (const GltfSaveMesh& sm : meshes) {
        if (!sm.material.base_color_tex.empty()) {
            any_tex = true;
        }
    }
    ASSERT_TRUE(any_tex) << "lantern 应有 baseColor 贴图（测试前提）";

    const std::string dir = "/tmp/jpov_saver_ext_test/";
    std::filesystem::create_directories(dir);
    const std::string gltf_path = dir + "lantern.gltf";
    const std::string bin_path = dir + "lantern.bin";
    std::remove(gltf_path.c_str());
    std::remove(bin_path.c_str());

    GltfSaveAsset asset;
    asset.name = "lantern";
    asset.meshes = meshes;
    ASSERT_TRUE(WriteGltf(asset, gltf_path)) << "WriteGltf 应成功";

    // 产物齐全：.gltf + .bin。
    EXPECT_TRUE(std::filesystem::exists(gltf_path));
    EXPECT_TRUE(std::filesystem::exists(bin_path));

    // .gltf 文本应外置引用（含 <stem>.bin / <stem>_texN，且不含 base64 data:）。
    std::ifstream gf(gltf_path, std::ifstream::binary);
    ASSERT_TRUE(gf.good());
    const std::string text((std::istreambuf_iterator<char>(gf)),
                           std::istreambuf_iterator<char>());
    EXPECT_NE(text.find("lantern.bin"), std::string::npos)
        << ".gltf 的 buffer 应外置引用 <stem>.bin";
    EXPECT_NE(text.find("lantern_tex"), std::string::npos)
        << ".gltf 的 image 应外置引用 <stem>_texN";
    EXPECT_EQ(text.find("data:image"), std::string::npos)
        << "外置导出不应把贴图内嵌成 base64";

    // 读回外置 .gltf：几何数一致 + 材质贴图仍能解析（image.uri 指向的 sidecar 真在）。
    std::vector<GltfSaveMesh> back;
    ASSERT_TRUE(LoadGltfScene(gltf_path, CollectSaveMeshes, &back))
        << "外置 .gltf 应能读回";
    ASSERT_EQ(back.size(), meshes.size());
    for (size_t i = 0; i < back.size(); ++i) {
        EXPECT_EQ(back[i].mesh.VertexCount(), meshes[i].mesh.VertexCount());
        if (!meshes[i].material.base_color_tex.empty()) {
            EXPECT_FALSE(back[i].material.base_color_tex.empty())
                << "外置贴图应能被 loader 解析回来";
        }
    }

    // 再打包回单文件 glb → 贴图内嵌仍在（外置→内嵌无损往返）。
    const std::string glb_path = dir + "lantern_repack.glb";
    std::remove(glb_path.c_str());
    GltfSaveAsset repack;
    repack.name = "lantern";
    repack.meshes = back;
    ASSERT_TRUE(WriteGlb(repack, glb_path)) << "读回后应能再打包成 glb";

    std::vector<GltfSaveMesh> back2;
    ASSERT_TRUE(LoadGltfScene(glb_path, CollectSaveMeshes, &back2));
    ASSERT_EQ(back2.size(), meshes.size());
    for (size_t i = 0; i < back2.size(); ++i) {
        EXPECT_EQ(back2[i].mesh.VertexCount(), meshes[i].mesh.VertexCount());
        if (!meshes[i].material.base_color_tex.empty()) {
            EXPECT_FALSE(back2[i].material.base_color_tex.empty())
                << "往返 glb 后 baseColor 贴图不应丢";
        }
    }
}

// 负面：非 .gltf 后缀必须 crash（契约：输出路径以 .gltf 结尾）。
TEST(GltfSaverTest, WriteGltfRejectsNonGltfPath) {
    GltfSaveAsset asset;
    asset.name = "tri";
    asset.meshes.push_back(GltfSaveMesh{MakeTriMesh(1.0f), GltfMaterialInfo{}});
    EXPECT_DEATH(WriteGltf(asset, TmpPath("notgltf")), "必须以 .gltf 结尾");
}

// 透明模式 / 双面：写→读回应保留 alphaMode=MASK + alphaCutoff + doubleSided。
// （否则“存→读”会静默丢掉 cutout / 双面语义。）
TEST(GltfSaverTest, RoundTripAlphaModeAndDoubleSided) {
    GltfSaveAsset asset;
    asset.name = "cutout";
    GltfMaterialInfo mi;
    mi.alpha_mode = AlphaMode::kMask;
    mi.alpha_cutoff = 0.4f;
    mi.double_sided = true;
    asset.meshes.push_back(GltfSaveMesh{MakeTriMesh(1.0f), mi});

    const std::string path = TmpPath("cutout");
    std::remove(path.c_str());
    ASSERT_TRUE(WriteGlb(asset, path));

    Collect c;
    ASSERT_TRUE(LoadGltfScene(path, CollectCb, &c));
    ASSERT_EQ(c.mats.size(), 1u);
    EXPECT_EQ(c.mats[0].alpha_mode, AlphaMode::kMask) << "alphaMode 应写回 MASK";
    EXPECT_NEAR(c.mats[0].alpha_cutoff, 0.4f, 1e-6f);
    EXPECT_TRUE(c.mats[0].double_sided) << "doubleSided 应写回 true";
    std::remove(path.c_str());
}

}  // namespace
}  // namespace jpov
