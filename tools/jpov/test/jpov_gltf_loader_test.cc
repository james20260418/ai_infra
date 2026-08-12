// JPOV glTF loader smoke test — CPU-only parse verification
//
// 验证 GltfLoader 能正确解析 pliers.gltf 并提取：
//   - 顶点数据（positions/normals/uvs + tangents + indices）
//   - 材质贴图路径
//
// 不涉及 GPU 渲染，纯 CPU 测试。

#include <cstdio>
#include <string>

#include <glog/logging.h>

#include "tools/jpov/src/gltf_loader.h"

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);

    jpov::MeshData mesh;
    jpov::GltfMaterialInfo mat;
    bool ok = jpov::LoadGltf(
        "tools/jpov/test/object3d/pliers_gltf/pliers.gltf",
        &mesh, &mat);
    CHECK(ok) << "LoadGltf returned false";

    LOG(INFO) << "vertices=" << mesh.VertexCount()
              << " indices=" << mesh.indices.size();
    LOG(INFO) << "hasNormal="
              << jpov::MeshHasFlag(mesh.flags, jpov::MeshVertexFlags::kNormal)
              << " hasUV="
              << jpov::MeshHasFlag(mesh.flags, jpov::MeshVertexFlags::kUV)
              << " hasTangent="
              << jpov::MeshHasFlag(mesh.flags, jpov::MeshVertexFlags::kTangent);

    CHECK_GT(mesh.VertexCount(), 0u);
    CHECK_GT(mesh.indices.size(), 0u);
    CHECK(jpov::MeshHasFlag(mesh.flags, jpov::MeshVertexFlags::kPosition));
    CHECK(jpov::MeshHasFlag(mesh.flags, jpov::MeshVertexFlags::kNormal));
    CHECK(jpov::MeshHasFlag(mesh.flags, jpov::MeshVertexFlags::kUV));
    CHECK(jpov::MeshHasFlag(mesh.flags, jpov::MeshVertexFlags::kTangent));

    CHECK(!mat.base_color_tex.empty());
    CHECK(!mat.normal_tex.empty());
    CHECK(!mat.metallic_roughness_tex.empty());

    LOG(INFO) << "baseColorTex=" << mat.base_color_tex;
    LOG(INFO) << "normalTex=" << mat.normal_tex;
    LOG(INFO) << "metallicRoughnessTex=" << mat.metallic_roughness_tex;

    LOG(INFO) << "TEST PASSED";
    return 0;
}
