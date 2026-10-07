// JPOV glTF loader — 内嵌 bufferView 贴图 GLB 加载验证
//
// 验证 GltfLoader 能正确解析【贴图内嵌】的单文件 GLB（投石车 catapult.glb，
// Tripo 生成）：
//   - 顶点数据（positions/normals/uvs + tangents + indices）
//   - 材质贴图：内嵌 bufferView 图片以**原始编码字节**放进 GltfTextureRef.bytes
//     （**不再经临时文件**）
//
// 关键：这个 GLB 的 baseColorTexture 是内嵌 bufferView（无 uri），
// 历史上 loader 只认外部 uri 导致加载失败；本测试确保内嵌路径可用。
//
// 注意：必须验证内嵌字节能被 stbi_load_from_memory 真正解码（而不是只检查非空
// —— 非空但内容是坏的 = 恒真断言）。
//
// 纯 CPU 测试，不涉及 GPU 渲染。

#include <cstdio>
#include <string>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"
#include "tools/jpov/src/gltf_loader.h"

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);

    const std::string kGlb =
        "tools/jpov/assets/models/samples/catapult/catapult.glb";

    jpov::MeshData mesh;
    jpov::GltfMaterialInfo mat;
    bool ok = jpov::LoadGltf(kGlb, &mesh, &mat);
    CHECK(ok) << "LoadGltf 失败（内嵌贴图 GLB 应能加载）: " << kGlb;

    LOG(INFO) << "vertices=" << mesh.VertexCount()
              << " indices=" << mesh.indices.size();

    // 几何：必须有 position / normal / uv（JPOV 静态 PBR 硬性前置）。
    CHECK_GT(mesh.VertexCount(), 0u);
    CHECK_GT(mesh.indices.size(), 0u);
    CHECK(jpov::MeshHasFlag(mesh.flags, jpov::MeshVertexFlags::kPosition));
    CHECK(jpov::MeshHasFlag(mesh.flags, jpov::MeshVertexFlags::kNormal));
    CHECK(jpov::MeshHasFlag(mesh.flags, jpov::MeshVertexFlags::kUV));

    // 内嵌 baseColor 贴图：应为**内嵌 ref**（bytes 非空、uri 空、key 非空），不再落盘。
    // 投石车 GLB 是 pbr:false 生成，只有 baseColor，故 normal/mr 贴图为空。
    CHECK(!mat.base_color_tex.empty()) << "应有 baseColor 贴图";
    CHECK(mat.base_color_tex.is_embedded())
        << "内嵌 bufferView 贴图应以内存字节交付（不再经临时文件）";
    CHECK(mat.base_color_tex.uri.empty()) << "内嵌图不应有外部 uri";
    CHECK(!mat.base_color_tex.key.empty()) << "内嵌图应有去重 key（内容哈希）";

    // 关键断言：内嵌字节必须能被 stbi_load_from_memory 真正解码为真实图像
    // （防止“非空但内容是坏的”恒真断言）。
    int w = 0, h = 0, c = 0;
    unsigned char* px = stbi_load_from_memory(
        mat.base_color_tex.bytes.data(),
        static_cast<int>(mat.base_color_tex.bytes.size()), &w, &h, &c, 4);
    CHECK(px != nullptr)
        << "内嵌贴图字节应能解码（"
        << (stbi_failure_reason() ? stbi_failure_reason() : "n/a") << "）";
    stbi_image_free(px);
    CHECK_GT(w, 0);
    CHECK_GT(h, 0);
    LOG(INFO) << "embedded texture decoded from memory: " << w << "x" << h
              << " ch" << c;

    LOG(INFO) << "TEST PASSED (embedded GLB texture)";
    return 0;
}
