// JPOV 软体仿真器 — 主入口（某时刻的 mesh，经一小段时间的动力学后，变成新的 mesh）
//
// ============================ 本文件回答的唯一问题 ============================
//
//     SoftMeshSimulator::Step(mesh, dt)  →  新的 mesh
//
// 即：一个三角形网格（MeshData），经过 dt 秒的动力学演化，变成一个新的三角形网格。
// 这是软体仿真链条的【唯一主入口】。查看器（soft_mesh_simulator.sh 产物）只负责
// 把「上一步的输出」当成「下一步的输入」逐帧喂给本接口，并把结果画出来——它不
// 持有任何物理状态，物理状态全部封在本类内部。
//
// ============================ 当前阶段（M0）======================
//
// **只做静态展示**：动力学尚未实现，Step() 是显式的恒等桩（恒等映射 + 计数）。
// 查看器已经把「渲染一帧 mesh」这条链路完全打通（加载 → 地面/光照 → 相机 →
// UI → OneIteration），后续每加一条物理功能（重力 / 弹簧约束 / 地面碰撞 / …）
// 都是在本文件里往里填，查看器一行都不用改。
//
// 之所以先立这个骨架，是为了让「物理」和「显示」从一开始就完全解耦：
//   - 物理：纯 CPU、零 GL、可单测（soft_mesh_sim_test.cc）
//   - 显示：JPOV 渲染管线，物理只是喂给它的一个 MeshData
//
// ============================ 设计约定 ============================
//
// 无状态查询：任何 Get* 接口都不改变物理状态（查看器画 UI 时随便调）。
// 不可变性：Step(dt) 只推进内部状态并返回结果，输入 mesh 不被修改。
// 单位铁律：长度统一为**米**。glTF 资产在加载边界已换算（见 gltf_loader.cc），
//   本类不做任何单位推断。dt 单位为**秒**。

#ifndef JPOV_DEMO_SOFT_MESH_SIM_H_
#define JPOV_DEMO_SOFT_MESH_SIM_H_

#include <cstddef>
#include <vector>

#include "geom/common/vec.h"
#include "tools/jpov/interface/mesh.h"

namespace jpov_soft {

// 仿真体在某一时刻的取景包围盒（纯查询，无副作用）。
//
// 查看器用它做「相机是否要跟着变形体缩放」这类判断；当前 M0 阶段几乎恒等于
// 输入网格的包围盒。带 valid 标志：空网格时 min/max 未定义，调用方必须先看标志。
struct SimBounds {
    // 说明：这里用完整拼写 geom::Vec3<float>（而非 jpov 命名空间内的 Vec3f 别名），
    // 因为本头不属于 jpov 命名空间、需要自带类型来源，避免被别名可见性牵连。
    geom::Vec3<float> min = geom::Vec3<float>(0.0f, 0.0f, 0.0f);
    geom::Vec3<float> max = geom::Vec3<float>(0.0f, 0.0f, 0.0f);
    bool valid = false;
};

// 软体仿真器。
//
// 一个实例 = 一个被仿真的网格体。生命周期：
//   Init(mesh)        —— 初始化内部状态（顶点副本、拓扑、约束……）
//   Step(dt) * N      —— 按 1/60 s 等外部时钟反复推进
//   Bounds()          —— 取值景包围盒
//   Reset()           —— 回到绑定姿态（bind pose）并清零时钟
//
// 所有物理常量都以命名常量/成员暴露，便于后续 UI 滑条或 headless 批量实验覆盖；
// 当前 M0 阶段只有一个时间常量。
class SoftMeshSimulator {
public:
    // 外部时钟的一个标准步长：1/60 秒（60 Hz）。
    //
    // 这是查看器与 headless 序列共用的固定步长。放在本类而非查看器里，是为了
    // 让「物理怎么走」这件事完全归物理模块所有（查看器只读不改）。
    static constexpr double kDefaultDt = 1.0 / 60.0;

    SoftMeshSimulator() = default;

    // 用给定网格初始化仿真体。可重复调用（等价于 Reset 到新网格）。
    //
    // 保存输入的顶点/索引副本作为**绑定姿态**：Reset() 回到这里。
    // 输入 mesh 不被修改；不要求有法线/UV（物理只关心位置，法线在显示侧重算）。
    void Init(const jpov::MeshData& mesh);

    // ⭐ 主入口：网格经 dt 秒动力学后，变成新的网格。
    //
    // 返回**按值**的新网格，调用方可直接交给渲染/下一步。
    //   - dt 必须 > 0，否则 CHECK 崩溃（静默跳帧会掩盖时间 bug）
    //   - 内部按 dt 推进时间累加器与步数计数
    //   - M0 阶段：恒等映射（位置原样返回），拓扑/属性全部保留
    jpov::MeshData Step(double dt);

    // 当前网格（最近一次 Step 的输出；未 Step 过则等于 Init 的网格）。
    // 返回常量引用以避免查看器每帧拷贝；查看器只读。
    const jpov::MeshData& mesh() const { return mesh_; }

    // 已仿真时间（秒），= 所有 Step(dt) 之和。
    double time() const { return time_; }
    // 已执行步数（次）。
    size_t step_count() const { return step_count_; }

    // 顶点数 / 三角形数（无索引网格按 positions/3 记三角形）。
    size_t vertex_count() const { return vertex_count_; }
    size_t triangle_count() const { return triangle_count_; }

    // 当前取景包围盒（纯查询）。
    SimBounds Bounds() const;

    // 回到绑定姿态（Init 时的网格）并清零时间/步数计数。
    // 与 Init 的区别：保留已初始化标记——未 Init 过时 Reset 是 no-op。
    void Reset();

private:
    // ── 物理状态（当前 M0 阶段仅"当前网格 + 时钟"；后续物理量都加在这里）──
    // 说明：把「当前网格」当作唯一事实源，而不是另外维护一份顶点数组，
    // 是为了让 Step 的输出与状态永远一致——查看器拿到的就是仿真器认的那份。
    jpov::MeshData mesh_;      // 当前网格
    jpov::MeshData bind_mesh_; // 绑定姿态（Reset 用）

    double time_ = 0.0;        // 已仿真时间（秒）
    size_t step_count_ = 0;    // 已执行步数

    size_t vertex_count_ = 0;  // 缓存（每帧 UI 要显示，避免反复算）
    size_t triangle_count_ = 0;

    bool inited_ = false;      // 是否已 Init（Reset 的前置校验）
};

}  // namespace jpov_soft

#endif  // JPOV_DEMO_SOFT_MESH_SIM_H_
