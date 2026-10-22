// gltf_unit_test.cc — glTF 长度单位消费验证（CPU-only，无需 GL）
//
// 2026-10-22 Danis：**glb 里的单位要消费，进 JPOV 一律换成米；存的也一律是米。**
//
// 覆盖：
//   ① 合成 .gltf：asset.extras.jpov_unit = "cm" → 顶点 ×0.01；
//   ② 合成 .gltf：node 用 `matrix`（缩放 2）→ 顶点 ×2（此前 matrix 被完全忽略）；
//   ③ 显式 GltfLoadOptions::unit_meters 覆盖文件自述；
//   ④ 真资产 yoga_pants.glb：unit=0.01 的包围盒 = unit=1 的 1/100（非恒真对照）；
//   ⑤ saver：写出的 glb 里 asset.extras 自述 jpov_unit = "m"。
//
// 每个用例都是「量出来的数」对拍，不是只看接口返回 true。

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/interface/mesh.h"
#include "tools/jpov/src/gltf_loader.h"
#include "tools/jpov/src/gltf_saver.h"

namespace {

// 一个 3 顶点三角形（无索引）的 POSITION 缓冲： (0,0,0) (1,0,0) (0,2,0)
constexpr const char* kBase64Triangle =
    "AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAAEAAAAAA";

// 生成一个最小 .gltf。asset_extra / node_extra 由调用方拼进 JSON。
std::string MakeGltf(const std::string& asset_extras_json,
                     const std::string& node_extra_json) {
    std::string json = R"({
  "asset": {"version": "2.0")";
    json += asset_extras_json;
    json += R"(},
  "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,)";
    json += kBase64Triangle;
    json += R"("}],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}],
  "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3,
                 "type": "VEC3", "min": [0, 0, 0], "max": [1, 2, 0]}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
  "nodes": [{"mesh": 0)";
    json += node_extra_json;
    json += R"(}],
  "scenes": [{"nodes": [0]}],
  "scene": 0
})";
    return json;
}

// 把 JSON 写到临时文件，返回路径。
std::string WriteTempGltf(const std::string& name, const std::string& json) {
    const std::string path = "/tmp/jpov_gltf_unit_test_" + name + ".gltf";
    std::ofstream out(path, std::ios::binary);
    CHECK(out.good()) << "无法写入临时文件: " << path;
    out << json;
    out.close();
    return path;
}

struct LoadedStats {
    bool ok = false;
    float max_y = 0.0f;
    float max_x = 0.0f;
    size_t vertices = 0;
};

LoadedStats LoadStats(const std::string& path, const jpov::GltfLoadOptions& opt) {
    LoadedStats st;
    struct Ctx {
        LoadedStats* st;
    } ctx{&st};
    const auto cb = [](const jpov::GltfMeshEntry* entry, void* user) {
        Ctx* c = static_cast<Ctx*>(user);
        for (const jpov::Vec3f& p : entry->mesh.positions) {
            c->st->max_y = std::max(c->st->max_y, p.y());
            c->st->max_x = std::max(c->st->max_x, p.x());
            ++c->st->vertices;
        }
    };
    st.ok = jpov::LoadGltfScene(path, cb, &ctx, opt);
    return st;
}

}  // namespace

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);

    // ---- ① 文件自述单位：cm ----
    {
        const std::string p = WriteTempGltf(
            "cm", MakeGltf(R"(, "extras": {"jpov_unit": "cm"})", ""));
        const LoadedStats st = LoadStats(p, {});
        CHECK(st.ok) << "加载失败: " << p;
        CHECK_EQ(st.vertices, 3u);
        // 顶点最高 y = 2（源）→ 0.02 米
        CHECK(std::fabs(st.max_y - 0.02f) < 1e-6f)
            << "jpov_unit=cm 应把 2 变成 0.02，实际 " << st.max_y;
        LOG(INFO) << "① asset.extras.jpov_unit=cm → max_y=" << st.max_y;
    }

    // ---- ② node.matrix（缩放 2）必须被消费 ----
    {
        // 列主序：对角 = 2，最后一列平移 0。
        const std::string mat =
            R"(, "matrix": [2,0,0,0, 0,2,0,0, 0,0,2,0, 0,0,0,1])";
        const std::string p = WriteTempGltf("matrix", MakeGltf("", mat));
        const LoadedStats st = LoadStats(p, {});
        CHECK(st.ok) << "加载失败: " << p;
        CHECK(std::fabs(st.max_y - 4.0f) < 1e-5f)
            << "node.matrix 缩放 2 应把 2 变成 4，实际 " << st.max_y;
        LOG(INFO) << "② node.matrix(scale=2) → max_y=" << st.max_y;
    }

    // ---- ③ 显式 unit_meters 覆盖文件自述 ----
    {
        const std::string p = WriteTempGltf(
            "override", MakeGltf(R"(, "extras": {"jpov_unit": "cm"})", ""));
        jpov::GltfLoadOptions opt;
        opt.unit_meters = 0.5f;
        const LoadedStats st = LoadStats(p, opt);
        CHECK(st.ok);
        CHECK(std::fabs(st.max_y - 1.0f) < 1e-6f)
            << "显式 unit=0.5 应覆盖文件自述 cm（2 → 1），实际 " << st.max_y;
        LOG(INFO) << "③ 显式 unit_meters=0.5 → max_y=" << st.max_y;
    }

    // ---- ④ 真资产：unit=0.01 的包围盒 = unit=1 的 1/100 ----
    {
        const std::string kPants =
            "tools/jpov/assets/models/clothing/yoga_pants/yoga_pants.glb";
        const LoadedStats one = LoadStats(kPants, {});
        CHECK(one.ok) << "加载失败: " << kPants;
        CHECK_GT(one.vertices, 0u);
        CHECK_GT(one.max_y, 0.1f);

        jpov::GltfLoadOptions opt;
        opt.unit_meters = 0.01f;
        const LoadedStats cm = LoadStats(kPants, opt);
        CHECK(cm.ok);
        const float ratio = cm.max_y / one.max_y;
        CHECK(std::fabs(ratio - 0.01f) < 1e-4f)
            << "unit=0.01 应把尺寸缩到 1/100，实际比值 " << ratio;
        LOG(INFO) << "④ yoga_pants: y=" << one.max_y << " → " << cm.max_y
                  << " (ratio=" << ratio << ")";
    }

    // ---- ⑤ saver 自述单位 = 米 ----
    {
        jpov::MeshData mesh;
        mesh.flags = static_cast<jpov::MeshVertexFlags>(
            static_cast<uint8_t>(jpov::MeshVertexFlags::kPosition) |
            static_cast<uint8_t>(jpov::MeshVertexFlags::kNormal));
        mesh.positions = {jpov::Vec3f(0, 0, 0), jpov::Vec3f(1, 0, 0),
                          jpov::Vec3f(0, 1, 0)};
        mesh.normals = {jpov::Vec3f(0, 0, 1), jpov::Vec3f(0, 0, 1),
                        jpov::Vec3f(0, 0, 1)};
        mesh.indices = {0, 1, 2};
        mesh.Validate();

        jpov::GltfSaveAsset asset;
        asset.name = "unit_test";
        asset.meshes.push_back(jpov::GltfSaveMesh{mesh, {}});
        const std::string out = "/tmp/jpov_gltf_unit_test_saved.glb";
        CHECK(jpov::WriteGlb(asset, out)) << "WriteGlb 失败: " << out;

        std::ifstream in(out, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
        CHECK(bytes.find("\"jpov_unit\":\"m\"") != std::string::npos ||
              bytes.find("\"jpov_unit\": \"m\"") != std::string::npos)
            << "saver 应在 asset.extras 自述 jpov_unit=m";
        LOG(INFO) << "⑤ saver 写出 " << bytes.size() << " 字节，含 jpov_unit=m";
    }

    // ---- ⑥ 单位字符串解析 ----
    {
        CHECK(std::fabs(jpov::ParseGltfUnitMeters("cm") - 0.01f) < 1e-9f);
        CHECK(std::fabs(jpov::ParseGltfUnitMeters("MM") - 0.001f) < 1e-9f);
        CHECK(std::fabs(jpov::ParseGltfUnitMeters("inch") - 0.0254f) < 1e-9f);
        CHECK(std::fabs(jpov::ParseGltfUnitMeters("0.25") - 0.25f) < 1e-9f);
        CHECK_EQ(jpov::ParseGltfUnitMeters("bogus"), 0.0f);
        LOG(INFO) << "⑥ 单位字符串解析 OK";
    }

    LOG(INFO) << "TEST PASSED (gltf units → meters)";
    return 0;
}
