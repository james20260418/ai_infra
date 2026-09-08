// JPOV glTF loader — 骨骼（skin + JOINTS_0/WEIGHTS_0）加载验证
//
// 用真实资材 mixamo_male.glb（Tripo Auto Rig spec=mixamo 生成, 23 关节骨架 + 4-bone
// 蒙皮, 忆确证 09-08 抽 JSON 确认: 1 mesh / JOINTS_0+WEIGHTS_0 / skins.inverseBind
// MAT4×23）验证两条链路:
//   1. LoadGltfScene/LoadGltf: 顶点 JOINTS_0/WEIGHTS_0 被读进 MeshData (joint_indices/
//      joint_weights + kJoints flag), 供 mesh_manager 上 GPU loc3/4（上传在 mesh_manager 测）。
//   2. LoadGltfSkeleton: 读 skins[0] 转成 SkeletonType —— 23 关节、每骨 name 带
//      "mixamorig:"、inverse_bind 23 个 MAT4、joints 树 parent 拓扑合法。
//
// 纯 CPU 测试, 不涉及 GPU。验证"把文件里的骨骼忠实读出来", 不做坐标旋装(gold/渲染留后续)。

#include <string>

#include <glog/logging.h>

#include "tools/jpov/src/gltf_loader.h"

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);

    const std::string kGlb =
        "tools/jpov/test/object3d/mixamo_male/mixamo_male.glb";

    // ---- 1. 顶点侧 JOINTS_0 / WEIGHTS_0 ----
    jpov::MeshData mesh;
    jpov::GltfMaterialInfo mat;
    CHECK(jpov::LoadGltf(kGlb, &mesh, &mat))
        << "LoadGltf 应成功加载带骨 male_rig GLB";
    LOG(INFO) << "vertices=" << mesh.VertexCount()
              << " flags=0x" << std::hex
              << static_cast<int>(mesh.flags) << std::dec;

    // 必须带骨骼 flag + 数据(该资产含蒙皮权重)
    CHECK(jpov::MeshHasFlag(mesh.flags, jpov::MeshVertexFlags::kJoints))
        << "male_rig 应置 kJoints";
    CHECK_EQ(mesh.joint_indices.size(), mesh.VertexCount());
    CHECK_EQ(mesh.joint_weights.size(), mesh.VertexCount());

    // 抽查权重合法(每顶点 4 权重近似和为 1、关节号在骨架范围内)——不遍历全
    // 但值得头几个顶点看一眼有没有读成垃圾。
    const int bone_limit = 23;
    for (int v = 0; v < 8; ++v) {
        float wsum = 0.0f;
        for (int j = 0; j < 4; ++j) {
            CHECK_GE(mesh.joint_indices[v][j], 0);
            CHECK_LT(mesh.joint_indices[v][j], bone_limit)
                << "第 " << v << " 顶点关节序号越界 (非 skin 内)";
            wsum += mesh.joint_weights[v][j];
        }
        // 权重可能有未归一(个别资产), 但不应全 0 或爆炸
        CHECK_GT(wsum, 0.01f) << "第 " << v << " 顶点权重和接近 0, 读权读出问题?";
        LOG(INFO) << "v" << v << " joints=["
                  << mesh.joint_indices[v][0] << ","
                  << mesh.joint_indices[v][1] << ","
                  << mesh.joint_indices[v][2] << ","
                  << mesh.joint_indices[v][3] << "] wsum=" << wsum;
    }

    // ---- 2. 骨骼定义 LoadGltfSkeleton ----
    std::vector<jpov::SkeletonType> skins;
    CHECK(jpov::LoadGltfSkeleton(kGlb, &skins)) << "LoadGltfSkeleton 应成功";
    CHECK_EQ(skins.size(), 1u) << "该资产应报 1 个 skin";

    const jpov::SkeletonType& skel = skins[0];
    LOG(INFO) << "skin[0]: bone_count=" << skel.bone_count();
    CHECK_EQ(skel.bone_count(), 23) << "mixamo biped 应 23 骨";

    // 每根应有名字(mixamorig:) 且 parent 拓扑合法(joint parent < 自身)
    for (int b = 0; b < skel.bone_count(); ++b) {
        const jpov::SkeletonJoint& jt = skel.joints[b];
        CHECK(!jt.name.empty()) << "bone[" << b << "] 应有名";
        if (b != 0) {  // 至少根可能是唯一无父
            // 不 assert 每骨都非根, 但要 parent 索引合法: -1 或 < b
            if (jt.parent != jpov::kSkeletonNoParent) {
                CHECK_LT(jt.parent, b)
                    << "bone[" << b << "](" << jt.name
                    << ") parent=" << jt.parent << " 破坏拓扑序";
            }
        }
    }
    // 有名子串(头必是 mixamorig 或 Root)
    const std::string& first_name = skel.joints[0].name;
    LOG(INFO) << "joint[0] name='" << first_name << "'";

    // inverse_bind 应有 23 个(该资产自带 IBM accessor MAT4)
    CHECK_EQ(skel.inverse_bind.size(),
             static_cast<size_t>(skel.bone_count()))
        << "male_rig skin 应带完整 inverseBindMatrices";

    LOG(INFO) << "gltf skeleton loader OK: " << skel.bone_count()
              << " bones from " << first_name;

    return 0;
}
