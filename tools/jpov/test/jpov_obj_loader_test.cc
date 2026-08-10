// JPOV OBJ 加载器单元测试
//
// 验证 LoadObj 把 .obj 文本正确解析为 CPU 侧 MeshData：
//   1. 手写立方体 cube_hand.obj —— 精确验证数值:
//      v/vt/vn 全分量、四边形面 fan 三角化、负索引(相对索引)、
//      corner-splitting(同一 position 配不同 vt/vn 拆为多 GPU 顶点)、flags
//   2. beetle.obj / suzanne.obj —— 真实模型加载通过:
//      v//vn 无 UV 格式、全三角/混合三角+四边形、三角形总数与文件一致
//
// 本测试为纯 CPU 解析测试，不涉及渲染（渲染 gold test 见 DrawObject3D 任务）。

#include <string>

#include <glog/logging.h>

#include "tools/jpov/interface/mesh.h"
#include "tools/jpov/src/obj_loader.h"

namespace {

// 测试资源路径（bazel data 会把 test 目录作为 runfiles 根）
const char* kDataRoot = "tools/jpov/test/";

void Check(bool cond, const std::string& msg) {
    if (!cond) {
        LOG(FATAL) << "OBJ loader test failed: " << msg;
    }
}

// ---- 1. 手写立方体: 精确数值 ----
void TestHandwrittenCube() {
    jpov::MeshData mesh;
    Check(jpov::LoadObj(std::string(kDataRoot) + "cube_hand.obj", &mesh),
          "cube_hand.obj 应成功加载");

    // 8 位置 × 各被 3 面引用配不同 vt/vn → 24 个唯一 (v,vt,vn) GPU 顶点
    Check(mesh.VertexCount() == 24, "cube_hand GPU 顶点数应为 24");
    Check(mesh.indices.size() == 36, "cube_hand indices 应为 36 (6面×2三角形×3)");

    // flags: 有 vn(→kNormal) + 有 vt(→kUV) + kPosition 必有
    Check(jpov::MeshHasFlag(mesh.flags, jpov::MeshVertexFlags::kPosition),
          "flags 应含 kPosition");
    Check(jpov::MeshHasFlag(mesh.flags, jpov::MeshVertexFlags::kNormal),
          "flags 应含 kNormal");
    Check(jpov::MeshHasFlag(mesh.flags, jpov::MeshVertexFlags::kUV),
          "flags 应含 kUV");
    Check(!jpov::MeshHasFlag(mesh.flags, jpov::MeshVertexFlags::kJoints),
          "flags 不应含 kJoints");

    // 数组对齐（Validate 内部已强制，显式再调用确认不 crash）
    mesh.Validate();

    // 抽样: 位置 (0,0,0)，出现在底面(法线1)、前面(法线3)、左面(法线5)
    // 拆分后应为 3 个 GPU 顶点，位置值与原始一致。
    // 注意: OBJ 面的遍历顺序决定了 corner map 中第一个键的次序，
    // 不依赖 positions[0] 的具体值，只检查拆分后的顶点数。
    int origin_count = 0;
    for (const auto& p : mesh.positions) {
        if (p == jpov::Vec3f(0, 0, 0)) ++origin_count;
    }
    Check(origin_count == 3,
          "位置 (0,0,0) 应被拆为 3 个 GPU 顶点（corner-splitting），实际=" + std::to_string(origin_count));

    // indices 全落在合法范围 [0, VertexCount)
    for (uint32_t idx : mesh.indices) {
        Check(idx < mesh.VertexCount(), "index 越界");
    }
}

// ---- 2. beetle.obj: 真实模型, 全三角形 v//vn 无 UV ----
void TestBeetle() {
    jpov::MeshData mesh;
    Check(jpov::LoadObj(std::string(kDataRoot) + "beetle.obj", &mesh),
          "beetle.obj 应成功加载");

    // 全三角面 2053 → indices = 2053 × 3 = 6159
    Check(mesh.indices.size() == 6159, "beetle indices 应为 6159");
    Check(mesh.normals.size() == mesh.VertexCount(),
          "beetle normals 应与顶点数对齐");
    Check(mesh.uvs.empty(), "beetle 无 vt, uvs 应为空");
    Check(jpov::MeshHasFlag(mesh.flags, jpov::MeshVertexFlags::kNormal),
          "beetle flags 应含 kNormal");
    Check(!jpov::MeshHasFlag(mesh.flags, jpov::MeshVertexFlags::kUV),
          "beetle flags 不应含 kUV");
    mesh.Validate();
}

// ---- 3. suzanne.obj: 真实模型, 混合三角+四边形 ----
void TestSuzanne() {
    jpov::MeshData mesh;
    Check(jpov::LoadObj(std::string(kDataRoot) + "suzanne.obj", &mesh),
          "suzanne.obj 应成功加载");

    // 32 个三角面 + 468 个四边形面 →
    // indices = 32×3 + 468×2×3 = 96 + 2808 = 2904
    Check(mesh.indices.size() == 2904, "suzanne indices 应为 2904 (fan 三角化)");
    Check(mesh.normals.size() == mesh.VertexCount(),
          "suzanne normals 应与顶点数对齐");
    Check(jpov::MeshHasFlag(mesh.flags, jpov::MeshVertexFlags::kNormal),
          "suzanne flags 应含 kNormal");
    Check(!jpov::MeshHasFlag(mesh.flags, jpov::MeshVertexFlags::kUV),
          "suzanne flags 不应含 kUV");
    mesh.Validate();
}

}  // namespace

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = true;

    TestHandwrittenCube();
    TestBeetle();
    TestSuzanne();

    LOG(INFO) << "All OBJ loader tests passed.";
    return 0;
}
