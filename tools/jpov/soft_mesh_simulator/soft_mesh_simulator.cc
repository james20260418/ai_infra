// JPOV 软体仿真器 — 实现（见 soft_mesh_simulator.h 的文件头）
//
// 纯 CPU / GL-free：本文件不 include 任何 GL 头，只碰 MeshData 与几何数学，
// 因此可以被单测直接跑（soft_mesh_simulator_test.cc）。

#include "tools/jpov/soft_mesh_simulator/soft_mesh_simulator.h"

#include <algorithm>
#include <limits>

#include <glog/logging.h>

namespace jpov {
namespace soft_mesh_simulator {

void Simulator::Init(const jpov::MeshData& mesh) {
    mesh.Validate();  // 长度/属性一致性：非法输入在这里就崩，不带进物理

    // 绑定姿态 = 输入网格的副本；当前网格也从绑定姿态起步。
    bind_mesh_ = mesh;
    mesh_ = mesh;

    vertex_count_ = mesh_.positions.size();
    // 无索引网格按「每 3 个顶点一个三角形」计（与渲染的 triangle list 语义一致）。
    triangle_count_ = mesh_.indices.empty() ? (vertex_count_ / 3)
                                            : (mesh_.indices.size() / 3);

    time_ = 0.0;
    step_count_ = 0;
    inited_ = true;

    LOG(INFO) << "soft_mesh_simulator::Init 顶点 " << vertex_count_
              << " / 三角形 " << triangle_count_ << "（M0 静态阶段：Step 为恒等）";
}

jpov::MeshData Simulator::Step(double dt) {
    CHECK(inited_) << "soft_mesh_simulator::Step 调用前必须先 Init(mesh)";
    CHECK(dt > 0.0) << "soft_mesh_simulator::Step 要求 dt > 0，got " << dt
                    << "（静默跳过时间会掩盖时钟 bug，故直接崩溃）";

    // ── M0（静态阶段）：恒等桩。──
    //
    // 这里**故意什么都不做**：不做位置积分、不做约束投影。它的价值是先把
    // 「输入 mesh → 输出 mesh」这条接口钉死，让查看器/单测/headless 全部围绕
    // 它建好骨架。后续每加一条物理，都是替换注释下面这几行，接口不变。
    //
    // 下一步（M1）将在此加入：重力预测 → 约束投影 → 速度回写；
    // 届时需要本类额外持有：顶点速度数组、约束集（边/面）、拓扑邻接。
    //
    // 当前恒等映射下 mesh_ 不需要变，直接返回当前值即可（零拷贝路径）。

    // 时间与步数照常累加——即使动力学是恒等的，时钟也必须真实推进，
    // 否则查看器/测试无法区分"停住了"和"没在走"。
    time_ += dt;
    ++step_count_;

    return mesh_;
}

SimBounds Simulator::Bounds() const {
    SimBounds b;
    if (mesh_.positions.empty()) return b;  // valid = false

    constexpr float kBig = std::numeric_limits<float>::max();
    geom::Vec3<float> lo(kBig, kBig, kBig);
    geom::Vec3<float> hi(-kBig, -kBig, -kBig);
    for (const geom::Vec3<float>& p : mesh_.positions) {
        lo[0] = std::min(lo[0], p[0]);
        lo[1] = std::min(lo[1], p[1]);
        lo[2] = std::min(lo[2], p[2]);
        hi[0] = std::max(hi[0], p[0]);
        hi[1] = std::max(hi[1], p[1]);
        hi[2] = std::max(hi[2], p[2]);
    }
    b.min = lo;
    b.max = hi;
    b.valid = true;
    return b;
}

void Simulator::Reset() {
    if (!inited_) return;  // 未初始化过：no-op（幂等，便于查看器无脑调用）

    mesh_ = bind_mesh_;
    time_ = 0.0;
    step_count_ = 0;

    LOG(INFO) << "soft_mesh_simulator::Reset 已回到绑定姿态（时间/步数清零）";
}

}  // namespace soft_mesh_simulator
}  // namespace jpov
