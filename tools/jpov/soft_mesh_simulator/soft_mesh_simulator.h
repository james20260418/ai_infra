// JPOV 软体仿真器 — 主入口（某时刻的 mesh，经一小段时间的动力学后，变成新的 mesh）
//
// ============================ 本模块回答的唯一问题 ============================
//
//     Simulator::Step(mesh, dt)  →  新的 mesh
//
// 即：一个三角形网格（MeshData），经过 dt 秒的动力学演化，变成一个新的三角形网格。
// 这是软体仿真链条的【唯一主入口】。查看器只负责把「上一步的输出」当成「下一步的
// 输入」逐帧喂给本接口，并把结果画出来——它不持有任何物理状态，物理状态全部封在
// 本类内部。
//
// 本模块是**独立包**（tools/jpov/soft_mesh_simulator/），不依赖渲染层：
// 纯 CPU、零 GL、可单测。这是刻意的边界——显示（JPOV 渲染管线）与物理（本包）
// 各自独立演进，互不污染。
//
// ============================ 当前阶段（M2：重力 + 速度衰减）==============================
//
// 动力学**第一项**落地：只做**重力**与**速度指数衰减**两项，让网格能「基本地下坠」。
// 弹簧/自碰撞等顶点间力场尚未接入（见 DESIGN.md §7 的 M1/M3）——本阶段的力只有重力，
// 故 a(t) = g 是常量，积分器因此有闭式解，可被单测逐项校验（见 .cc）。
//
// 之所以先只做这两项，是为了在引入顶点间耦合（力场）之前，先把**积分器本身**
// 钉死并验收：子步切分、对称阻尼、速度-位置更新顺序。耦合力一上线，这些都会
// 被掩盖（力算错与积分器写错混在一起，很难二分定位）。
//
// ============================ 设计约定 ============================
//
// 无状态查询：任何 Get* 接口都不改变物理状态（查看器画 UI 时随便调）。
// 不可变性：Step(dt) 只推进内部状态并返回结果，输入 mesh 不被修改。
// 单位铁律：长度统一为**米**。glTF 资产在加载边界已换算（见 gltf_loader.cc），
//   本类不做任何单位推断。dt 单位为**秒**。
// 前置条件用 CHECK 崩，不藏隐式行为（没有"传 0 就表示自动"这类魔法默认值）。

#ifndef JPOV_SOFT_MESH_SIMULATOR_SOFT_MESH_SIMULATOR_H_
#define JPOV_SOFT_MESH_SIMULATOR_SOFT_MESH_SIMULATOR_H_

#include <cstddef>
#include <vector>

#include <glog/logging.h>

#include "geom/common/vec.h"
#include "tools/jpov/interface/mesh.h"

namespace jpov {
namespace soft_mesh_simulator {

// 仿真体在某一时刻的取景包围盒（纯查询，无副作用）。
//
// 查看器用它做「相机是否要跟着变形体缩放」这类判断；M2 起网格会在重力下
// 下坠/变形，包围盒随仿真推进而变化（未 Step 时等于绑定姿态的包围盒）。
// 带 valid 标志：空网格时 min/max 未定义，调用方必须先看标志。
struct SimBounds {
    // 说明：这里用完整拼写 geom::Vec3<float>（而非 jpov 命名空间内的 Vec3f 别名），
    // 因为本头需要自带类型来源，避免被别名可见性牵连。
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
// 仿真点（simulation point）：参与仿真的实际质点集合。它由初始化时的
// **长边加密**（见 DESIGN.md §3.2）从输入的原始网格生成——
//   - 原始顶点：输入网格自带的点
//   - 虚拟顶点：为「任一边长 > bind_distance*0.9」的边在中间等距插入的点
// 二者参与仿真时地位完全一样；但**提取变形 mesh 时只用原始顶点**。
//
// 所有物理常量都以命名常量/成员暴露，便于后续 UI 滑条或 headless 批量实验覆盖。
class Simulator {
public:
    // 外部时钟的一个标准步长：1/60 秒（60 Hz）。
    //
    // 这是查看器与 headless 序列共用的固定步长。放在本类而非查看器里，是为了
    // 让「物理怎么走」这件事完全归物理模块所有（查看器只读不改）。
    static constexpr double kDefaultDt = 1.0 / 60.0;

    // 默认关联距离 d（米）。见 DESIGN.md §5（当前为全局常量，非滑条）。
    static constexpr float kDefaultBindDistance = 0.1f;

    // 默认重力加速度（m/s²），方向 -Y（DESIGN.md §1.2 第 3 项；地面在下方）。
    static constexpr float kDefaultGravity = 9.8f;

    // 重力滑条范围（m/s²）。下限 > 0（0 会让本阶段退化成「匀速直线」没意义；
    // 若将来要「关重力」，应由调用方显式表达，而不是把 0 当作魔法值）。
    static constexpr float kMinGravity = 1.0f;
    static constexpr float kMaxGravity = 20.0f;

    // 速度指数衰减系数 k（1/s）。DESIGN.md §2 第 3 项：V *= exp(-k*dt)。
    // 本阶段固定 0.1（Danis 2026-09-25 指定）—— 尚未做成滑条。
    static constexpr float kVelocityDamping = 0.1f;

    // 每个外部步（1/60 s）内的子步数。DESIGN.md §3.4：30 个子步（暴力解）。
    // 实际积分的步长 = dt / kSubsteps = 1/(60*30) = 5.5556e-4 s。
    static constexpr int kSubsteps = 30;

    Simulator() = default;

    // 用给定网格初始化仿真体。可重复调用（等价于 Reset 到新网格）。
    //
    // 保存输入的顶点/索引副本作为**绑定姿态**：Reset() 回到这里。
    // 输入 mesh 不被修改；不要求有法线/UV（物理只关心位置，法线在显示侧重算）。
    // 使用默认关联距离 kDefaultBindDistance。
    void Init(const jpov::MeshData& mesh);

    // 同上，但显式指定关联距离 d（米）。必须 > 0，否则 CHECK 崩溃。
    //
    // d 影响两件事：
    //   1. 长边加密（|edge| > d*0.9 的边会插入虚拟顶点）—— 决定仿真点数量
    //   2. （后续）关联邻居表 nb_list = { j : |v_j - v_i| <= d }
    void Init(const jpov::MeshData& mesh, float bind_distance);

    // ⭐ 主入口：网格经 dt 秒动力学后，变成新的网格。
    //
    // 返回**按值**的新网格，调用方可直接交给渲染/下一步。
    //   - dt 必须 > 0，否则 CHECK 崩溃（静默跳帧会掩盖时间 bug）
    //   - 内部把 dt 切成 kSubsteps 个子步，逐步积分（见 .cc 的积分器说明）
    //   - 本阶段（M2）：只施加**重力 + 速度衰减**，无顶点间力场
    jpov::MeshData Step(double dt);

    // 当前网格（最近一次 Step 的输出；未 Step 过则等于 Init 的网格）。
    // 返回常量引用以避免查看器每帧拷贝；查看器只读。
    const jpov::MeshData& mesh() const { return mesh_; }

    // 已仿真时间（秒），= 所有 Step(dt) 之和。
    double time() const { return time_; }
    // 已执行步数（次）。
    size_t step_count() const { return step_count_; }

    // 顶点数 / 三角形数（无索引网格按 positions/3 记三角形）。
    // ⚠️ 这是**原始输入网格**的计数（提取 mesh 的规模），不含虚拟顶点。
    size_t vertex_count() const { return vertex_count_; }
    size_t triangle_count() const { return triangle_count_; }

    // ── 仿真点（原始 + 虚拟）────────
    //
    // 当前关联距离 d（米），Init 时设定。
    float bind_distance() const { return bind_distance_; }

    // 仿真点总数（原始顶点 + 虚拟顶点）。M1 可视化与后续物理都基于它。
    size_t sim_point_count() const { return sim_positions_.size(); }
    // 其中原始顶点数（= 输入网格顶点数）。
    size_t original_point_count() const { return vertex_count_; }
    // 其中虚拟顶点数（加密插入）。
    size_t virtual_point_count() const {
        return sim_positions_.size() - vertex_count_;
    }

    // 仿真点的绑定姿态位置（索引 0..original_point_count()-1 为原始顶点，
    // 其后为虚拟顶点）。纯查询，返回常量引用。
    const std::vector<geom::Vec3<float>>& sim_positions() const {
        return sim_positions_;
    }

    // 该仿真点是否为虚拟顶点（加密插入）。
    // Pre-condition: sim_index < sim_point_count()；越界传入是调用方 bug → 崩。
    bool IsVirtualPoint(size_t sim_index) const {
        CHECK_LT(sim_index, sim_positions_.size())
            << "IsVirtualPoint 索引越界: " << sim_index << " >= "
            << sim_positions_.size();
        return sim_index >= vertex_count_;
    }

    // ── 物理参数（可被查看器滑条覆盖）──
    //
    // 重力加速度 g（m/s²，≥ 0），方向恒为 -Y。滑条范围 [kMinGravity, kMaxGravity]。
    // 查询与设置都走这里；设置时 CHECK 值域，不静默夹断（避免隐藏调用方的错值）。
    float gravity() const { return gravity_; }
    // Pre-condition: gravity >= 0（0 合法 = 无重力，供将来的开关用）；负值崩。
    void SetGravity(float gravity);

    // ── 仿真点速度（纯查询）──
    //
    // 与 sim_positions() 同序同长：索引 0..original_point_count()-1 = 原始顶点，
    // 其后为虚拟顶点。未 Step 过时全为 0。供单测/调试读取。
    const std::vector<geom::Vec3<float>>& sim_velocities() const {
        return sim_velocities_;
    }

    // 当前取景包围盒（纯查询）。
    SimBounds Bounds() const;

    // 回到绑定姿态（Init 时的网格）并清零时间/步数计数。
    // 与 Init 的区别：保留已初始化标记——未 Init 过时 Reset 是 no-op。
    void Reset();

private:
    // 由输入网格构建仿真点集合（含长边加密），返回虚拟点数量。
    // 纯 CPU，只读输入，无副作用（除了填充 sim_positions_/sim_edges_）。
    size_t BuildSimulationPoints(const jpov::MeshData& mesh, float d);

    // ── 物理状态（M2：位置 + 速度 + 时钟；后续物理量都加在这里）──
    // 说明：把「当前网格」当作唯一事实源，而不是另外维护一份顶点数组，
    // 是为了让 Step 的输出与状态永远一致——查看器拿到的就是仿真器认的那份。
    jpov::MeshData mesh_;      // 当前网格（原始顶点；提取用）
    jpov::MeshData bind_mesh_; // 绑定姿态（Reset 用）

    // 仿真点集合（原始 + 虚拟）的**当前位置**。索引 0..vertex_count_-1 = 原始顶点，
    // 之后为虚拟顶点。M1 可视化与后续物理都基于这份。
    // 不变量：其前 vertex_count_ 个元素始终与 mesh_.positions 一致（见 ExtractMesh）。
    std::vector<geom::Vec3<float>> sim_positions_;

    // 仿真点集合的**当前速度**（与 sim_positions_ 同序同长）。
    // 积分状态之一；Reset/Init 时清零。
    std::vector<geom::Vec3<float>> sim_velocities_;

    // 加密产生的虚拟点列表（仅 position），用于可视化/调试（蓝色点）。
    std::vector<geom::Vec3<float>> virtual_positions_;

    // 当前重力加速度（m/s²，≥ 0），方向 -Y。可由 SetGravity 覆盖。
    float gravity_ = kDefaultGravity;

    float bind_distance_ = kDefaultBindDistance;  // 关联距离 d（米）

    double time_ = 0.0;        // 已仿真时间（秒）
    size_t step_count_ = 0;    // 已执行步数

    size_t vertex_count_ = 0;  // 缓存（每帧 UI 要显示，避免反复算）
    size_t triangle_count_ = 0;

    bool inited_ = false;      // 是否已 Init（Reset 的前置校验）

    // 把仿真点当前的前 vertex_count_ 个位置写回 mesh_.positions（提取变形 mesh）。
    // 虚拟顶点不进入输出（DESIGN.md §3.2）。只改位置，拓扑/属性不动。
    void ExtractMesh();

    // 在给定子步长 dt_sub 上执行一次「重力 + 对称阻尼」的 leapfrog 积分。
    // 见 .cc 的完整公式与推导。
    void IntegrateSubstep(double dt_sub);
};

}  // namespace soft_mesh_simulator
}  // namespace jpov

#endif  // JPOV_SOFT_MESH_SIMULATOR_SOFT_MESH_SIMULATOR_H_
