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
// 故 a(x) 是常量（与位置无关），积分器因此有闭式解，可被单测逐项校验（见 .cc）。
//
// 之所以先只做这两项，是为了在引入顶点间耦合（力场）之前，先把**积分器本身**
// 钉死并验收：子步切分、对称阻尼、速度-位置更新顺序。耦合力一上线，这些都会
// 被掩盖（力算错与积分器写错混在一起，很难二分定位）。
//
// ⚠️ 积分器已按 **KDK（kick-drift-kick）** 结构写好：两个 kick 各用**自己时刻**
//    的加速度（前半用 a(x_old)，后半用 a(x_new)）。当前重力与位置无关，故两者取值
//    相同；接入位置相关力场后，「用新位置求第二个 a」即辛性的必要条件（详见 .cc）。
//
// ============================ M3：顶点间弹簧力场（Danis 自创）==============================
//
// 在 M2（重力+阻尼）基础上，接入**核心的顶点间弹性力场**（DESIGN.md §2，Danis 定稿）：
//   - 关联邻居表 nb_list：|v_j(0) - v_i(0)| <= d 的点对（纯欧氏距离，t=0 定死，永不更新）；
//   - 力：Fij = -[pij(t) - pij(0)] * F / max(|pij(0)|, d/10)，逐分量 clamp 到 ±F_max；
//   - 质量：m = M_total / N，加速度 a = (重力 + ΣFij) / m。
// 详见 .cc 的 ComputeAccel / BuildNeighborTable。
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
#include <cstdint>
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

    // 默认总质量 M_total（kg）。DESIGN.md §3.1：按顶点均分 m = M_total / N。
    // 质量只影响加速度 a = F/m（力的公式本身与质量无关）；越大越“重”、越不易被推动。
    static constexpr float kDefaultTotalMass = 1.0f;
    // 总质量滑条下限（kg）。DESIGN.md §3.1 护栏：防止 m 过小导致数值病态（§4.3）。
    static constexpr float kMinTotalMass = 1.0f;
    static constexpr float kMaxTotalMass = 200.0f;

    // 默认力系数 F（N）。DESIGN.md §2.3：含义 = “压缩比 1.0 时产生的力”。
    // 高模更硬 → 用户手动降 F（不自动归一化，§1.3）。
    static constexpr float kDefaultForceCoeff = 0.3f;
    // 力系数 F 的滑条范围（N）。DESIGN.md §5：0.01~20，跨 3 个数量级。
    // §5 要求用**指数坐标**滑条（低端也要有分辨率），UI 侧负责映射。
    static constexpr float kMinForceCoeff = 0.01f;
    static constexpr float kMaxForceCoeff = 20.0f;

    // 力的截断上限 F_max（N，绝对值）。DESIGN.md §2.5：兜底，防止 nb_list 不更新
    // 时 |Δp| 疯长导致力无限增长（数值灾难）。正常工况力 ~0.3N，不触发。
    static constexpr float kForceMax = 20.0f;

    // 每点的**关联邻居数上限**（保留最近的 kMaxNeighbors 个）。
    //
    // 性能关键（2026-09-26 Danis 实测）：d=0.1 对点距 ~0.01 的密模，平均邻居可达 300+，
    // 力循环 O(Σ_neighbors) × 30 子步 × 2 趟(KDK) → 60fps 下亿级/帧，明显卡顿。
    // 截到 20 个最近邻后，力计算量 ≈ 降 16×（且随机访存减少、cache 命中率改善）。
    //
    // 物理依据：近邻 |pij(0)| 小 ⇒ 力公式分母小 ⇒ 单位位移的力大 ⇒ 近邻主导力学贡献。
    // 0 = 不限制（保留 d 内全部邻居；仅供对照/调试，正常别用）。
    static constexpr size_t kMaxNeighbors = 20;

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

    // 默认地面高度 y（米）。地面是水平面（法线 +Y），低于它的顶点被投影回去。
    // 默认值 -3 与查看器地面 quad 的默认高度一致（view_config.h::MakeGroundQuad）。
    static constexpr float kDefaultGroundY = -3.0f;

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

    // 关联邻居表规模（纯查询，调试/面板用）：所有点各自关联数的总和（有向计数，
    // i→j 与 j→i 各计一次）。力场接上后这个数字直接决定每子步的计算量。
    size_t neighbor_pair_count() const {
        size_t total = 0;
        for (const auto& nb : neighbors_) total += nb.size();
        return total;
    }

    // 仿真点总数（原始顶点 + 虚拟顶点）。M1 可视化与后续物理都基于它。
    size_t sim_point_count() const { return sim_positions_.size(); }

    // 当前状态下某仿真点的**加速度** a(x_i)（纯查询，无副作用）。
    // 调试/单测/面板诊断用：直接暴露“该点当前受什么加速度”，使力的公式可被
    // 逐项验证（DESIGN §6 的单测需要它读力的截断效果）。
    // Pre-condition: idx < sim_point_count()；越界是调用方 bug → 崩。
    geom::Vec3<float> AccelAtPoint(size_t idx) const {
        CHECK_LT(idx, sim_positions_.size()) << "AccelAtPoint 索引越界: " << idx;
        return ComputeAccel(idx, sim_positions_[idx]);
    }
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
    // 总质量 M_total（kg，≥ kMinTotalMass）。按顶点均分 → 每点质量 m = M/N。
    // 只影响 a = F/m；不影响力的公式。
    float total_mass() const { return total_mass_; }
    // Pre-condition: mass 有限且 >= kMinTotalMass；否则崩（护栏见 §3.1/§4.3）。
    // 注意：改质量会改变每点质量 m（下次子步生效），但**不重建**关联表。
    void SetTotalMass(float mass);

    // 每点质量 m = M_total / N（kg）。N = 仿真点总数（含虚拟顶点）。
    // 查询时现算（纯查询）；N 为 0 时为 0。
    float point_mass() const {
        return sim_positions_.empty()
                   ? 0.0f
                   : total_mass_ / static_cast<float>(sim_positions_.size());
    }

    // 力系数 F（N，§2.3）。
    float force_coeff() const { return force_coeff_; }
    // Pre-condition: F 有限且 > 0；否则崩。负 F 会变成“反弹簧”（远离反而相吸）。
    void SetForceCoeff(float f);

    // 顶点间弹簧力场的**总开关**（DESIGN.md §1.2 机制 2）。
    // 关掉时：只保留重力（+阻尼），与 M2 行为一致——供单测隔离重力、以及
    // 用户对照“有力场 vs 无力场”。默认开。
    // 注意：关掉只是跳过力场累加，**不重建**关联表（开关瞬时生效）。
    bool spring_enabled() const { return spring_enabled_; }
    void SetSpringEnabled(bool enabled) { spring_enabled_ = enabled; }

    // 重力加速度 g（m/s²，≥ 0），方向恒为 -Y。滑条范围 [kMinGravity, kMaxGravity]。
    // 查询与设置都走这里；设置时 CHECK 值域，不静默夹断（避免隐藏调用方的错值）。
    float gravity() const { return gravity_; }
    // Pre-condition: gravity >= 0（0 合法 = 无重力，供将来的开关用）；负值崩。
    void SetGravity(float gravity);

    // 地面高度的 y（米）。地面为水平面（法线 +Y），低于它的顶点被**纯位置投影**回
    // 地面（不注入动能，见 .cc）。查询与设置都走这里。
    float ground_y() const { return ground_y_; }
    // 设置地面高度。CHECK 有限；无值域限制（地面可以在任意高度）。
    void SetGroundY(float ground_y);

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
    // 纯 CPU，只读输入，无副作用（除了填充 sim_positions_）。
    size_t BuildSimulationPoints(const jpov::MeshData& mesh, float d);

    // 由当前的 sim_positions_（绑定姿态）一次性构建**关联邻居表** nb_list
    // （DESIGN.md §2.1）：对每点 i，记录所有满足 |v_j(0) - v_i(0)| <= d 的 j
    // （纯欧氏距离，不看拓扑；在 t=0 定死，仿真中**永不更新**）。
    //
    // 同时缓存初始距离 |pij(0)| 进 nb_init_dist_（力的公式 §2.3 分母用）。
    // 复杂度 O(N²) 是刻意的“暴力解”（§3.4）；网格大时可后续加空间哈希。
    void BuildNeighborTable(float d);

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

    // 积分器第一趟的**半步速度**缓冲（与 sim_positions_ 同长）。
    // 作为成员复用：每个子步 resize 一次（不增不减时零开销），避免每子步堆分配。
    // 之所以需要它，是因为「前半 kick 必须全部基于旧位置算完，才能批量推进位置」——
    // 否则后算的点会读到已推进的邻居位置（半新半旧，见 IntegrateSubstep 注释）。
    std::vector<geom::Vec3<float>> v_half_buf_;

    // 当前重力加速度（m/s²，≥ 0），方向 -Y。可由 SetGravity 覆盖。
    float gravity_ = kDefaultGravity;

    // 总质量 M_total（kg）与力系数 F（N）。见 .h 顶部常量说明。
    float total_mass_ = kDefaultTotalMass;
    float force_coeff_ = kDefaultForceCoeff;
    // 弹簧力场总开关（默认开）；见 SetSpringEnabled。
    bool spring_enabled_ = true;

    // ── 关联邻居表（DESIGN.md §2.1；Init 时一次性建立，之后永不更新）──
    // neighbors_[i] = 与点 i 初始距离 <= d 的点的下标列表（不含 i 自身）。
    std::vector<std::vector<uint32_t>> neighbors_;
    // nb_init_dist_[i][k] = |pij(0)| = 点 i 与其第 k 个关联点 j = neighbors_[i][k]
    // 的**初始**距离（力的公式 §2.3 的分母用）。与 neighbors_ 同构同序。
    std::vector<std::vector<float>> nb_init_dist_;

    // 仿真点集合的**绑定姿态位置**（与 sim_positions_ 同序同长，含虚拟顶点）。
    // 力的公式里有 pij(0) = v_j(0) - v_i(0)（§2.2），需要绑定姿态坐标；
    // 而 sim_positions_ 会被 Step 推着走，bind_mesh_ 又不含虚拟顶点，
    // 故单独快照一份（Init/Reset 时更新）。
    std::vector<geom::Vec3<float>> bind_positions_;

    // 当前地面高度 y（米）。低于它的顶点被投影回地面（水平面，法线 +Y）。
    float ground_y_ = kDefaultGroundY;

    float bind_distance_ = kDefaultBindDistance;  // 关联距离 d（米）

    double time_ = 0.0;        // 已仿真时间（秒）
    size_t step_count_ = 0;    // 已执行步数

    size_t vertex_count_ = 0;  // 缓存（每帧 UI 要显示，避免反复算）
    size_t triangle_count_ = 0;

    bool inited_ = false;      // 是否已 Init（Reset 的前置校验）

    // 把仿真点当前的前 vertex_count_ 个位置写回 mesh_.positions（提取变形 mesh）。
    // 虚拟顶点不进入输出（DESIGN.md §3.2）。只改位置，拓扑/属性不动。
    void ExtractMesh();

    // 在给定子步长 dt_sub 上执行一次「重力 + 对称阻尼」的 leapfrog（KDK）积分，
    // 然后做地面投影（非穿透）。分两趟：先对全部点算 v_half/x_new，再对全部点
    // 用新位置求 a(x_new) 回写 v_new。见 .cc 的完整公式与推导。
    void IntegrateSubstep(double dt_sub);

    // ⭐ 该点在某位置受到的**总加速度** a(x) = (重力 + 弹簧力场) / m。
    //
    // 本阶段外力有两项（DESIGN.md §1.2）：
    //   1. 重力：常量 (0, -g, 0)；g=0 时为零。
    //   2. 顶点间弹簧力场（§2）：F_i = Σ_j Fij，其中
    //        Fij = -[pij(t) - pij(0)] * F / max(|pij(0)|, d/10)
    //      pij(t) = x_j(t) - x_i(t)（**当前**位置差），pij(0) 为初始位移（缓存于 nb_init_dist_）。
    //      Fij 逐分量 clamp 到 [-F_max, +F_max]（§2.5）。
    //      注意：力的公式不含质量；m 只在末尾做除法 a = F_total / m。
    //
    // ⚠️ 力是**点与点之间的耦合量**：a(x_i) 依赖**所有点**的当前位置。调用方
    //    （IntegrateSubstep 第二趟）必须保证本轮积分里所有点都已推进到 x_new，
    //    才能调本函数（这正是“KDK 两趟”结构的原因）。
    //
    // idx = 该点在 sim_positions_ 中的下标（力场需要它去查 neighbors_[idx]）。
    geom::Vec3<float> ComputeAccel(size_t idx, const geom::Vec3<float>& x) const;
};

}  // namespace soft_mesh_simulator
}  // namespace jpov

#endif  // JPOV_SOFT_MESH_SIMULATOR_SOFT_MESH_SIMULATOR_H_
