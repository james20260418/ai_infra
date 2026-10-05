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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <glog/logging.h>

#include "geom/3d/triangle_matcher_3d.h"
#include "geom/common/vec.h"
#include "tools/jpov/interface/mesh.h"

namespace jpov {
namespace soft_mesh_simulator {

// 旋转轴（即时操作参数；用 enum class 避免裸 0/1/2 传错轴）。
enum class Axis { kX = 0, kY = 1, kZ = 2 };

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
    // 默认值 20（Danis 2026-09-27 验收后定：投石车等高密度小资产合适）。
    static constexpr float kDefaultTotalMass = 20.0f;
    // 总质量滑条下限（kg）。DESIGN.md §3.1 护栏：防止 m 过小导致数值病态（§4.3）。
    static constexpr float kMinTotalMass = 1.0f;
    static constexpr float kMaxTotalMass = 200.0f;

    // 默认力系数 F（N）。DESIGN.md §2.3：含义 = “压缩比 1.0 时产生的力”。
    // 高模更硬 → 用户手动降 F（不自动归一化，§1.3）。
    // 默认值 3.0（Danis 2026-09-27 验收后定：投石车合适）。
    static constexpr float kDefaultForceCoeff = 3.0f;
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

    // 重力滑条范围（m/s²）。下限为负 = 允许「反重力 / 上浮」（供试验负重力，2026-10-05
    // Danis 定）；0 = 无重力（匀速直线）。
    static constexpr float kMinGravity = -3.0f;
    static constexpr float kMaxGravity = 10.0f;

    // 速度指数衰减系数 k（1/s），**默认值**。DESIGN.md §2 第 3 项：V *= exp(-k*dt)。
    // 运行时可经 SetVelocityDamping 覆盖（面板滑条），范围 [kMinDamping, kMaxDamping]。
    // 默认值 5（Danis 2026-09-27 验收后定：振荡快速平息、不呆滞）。
    static constexpr float kVelocityDamping = 5.0f;
    // 衰减系数滑条范围（1/s）。Danis 2026-09-26：k 越大衰减越快（exp(-k·dt)），
    // 默认 0.1 偏弱（10s 时间常数），放大到 10（0.1s 时间常数）可快速平息振荡。
    // 下限 > 0（0 = 无阻尼，弹簧会一直振荡不静；若要“关阻尼”由调用方显式表达）。
    static constexpr float kMinDamping = 0.1f;
    static constexpr float kMaxDamping = 10.0f;

    // 每个外部步（1/60 s）内的子步数。DESIGN.md §3.4：30 个子步（暴力解）。
    // 实际积分的步长 = dt / kSubsteps = 1/(60*30) = 5.5556e-4 s。
    static constexpr int kSubsteps = 30;

    // ── 人体排斥：buffer 值域与默认（Danis 2026-10-01 指定）──
    // buffer = 排斥目标离人体表面的最小距离（米）。0 = 直接贴表面。
    static constexpr float kMinBodyBuffer = 0.0f;
    static constexpr float kMaxBodyBuffer = 0.1f;
    static constexpr float kDefaultBodyBuffer = 0.01f;
    // 接触壳的极小裕量（米）：有符号距离 ≤ buffer + 本值即视为“接触”。用来吸收浮点
    // 噪声，保证恰好停在 buffer 上的点下一子步的**预速度约束**仍然生效（否则会出现
    // 一子步的位置锯齿）。0.1mm，远小于 buffer。
    static constexpr float kBodyContactMargin = 1.0e-4f;

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

    // 某点的关联邻居点 id 列表（纯查询；调试/单测用，用于验证关联表对称性）。
    // Pre-condition: idx < sim_point_count()。
    const std::vector<uint32_t>& neighbors_of(size_t idx) const {
        CHECK_LT(idx, neighbors_.size()) << "neighbors_of 索引越界: " << idx;
        return neighbors_[idx];
    }

    // 某点的关联邻居**初始距离** |pij(0)| 列表（与 neighbors_of 同构同序）。
    // 纯查询；调试/单测用。它是力公式 §2.3 的分母；因此**随即时缩放一起缩放**
    // （旋转 / 平移不改变模长）。
    // Pre-condition: idx < sim_point_count()。
    const std::vector<float>& neighbor_initial_distances_of(size_t idx) const {
        CHECK_LT(idx, nb_init_dist_.size())
            << "neighbor_initial_distances_of 索引越界: " << idx;
        return nb_init_dist_[idx];
    }

    // 关联表是否严格对称（i 关联 j ⇔ j 关联 i）——牛顿第三定律的必要条件。
    // 纯查询，O(Σ neighbors)；调试/单测用。
    bool neighbors_symmetric() const {
        for (size_t i = 0; i < neighbors_.size(); ++i) {
            for (uint32_t j : neighbors_[i]) {
                const auto& jn = neighbors_[j];
                if (std::find(jn.begin(), jn.end(), static_cast<uint32_t>(i)) ==
                    jn.end()) {
                    return false;
                }
            }
        }
        return true;
    }

    // 关联图的**连通分量数**（纯查询，并查集）。1 = 所有仿真点连成一张网。
    // 调试/单测用；也用于验证 Init 的连通性修复（EnsureNeighborGraphConnected）。
    size_t neighbor_component_count() const;

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

    // 速度指数衰减系数 k（1/s）。公式 V *= exp(-k·dt)；**k 越大衰减越快**。
    // 滑条范围 [kMinDamping, kMaxDamping] = [0.1, 10]。
    float velocity_damping() const { return velocity_damping_; }
    // Pre-condition: k 有限且 >= 0（0 = 无阻尼，合法但不静止）；负值崩。
    void SetVelocityDamping(float k);

    // 顶点间弹簧力场的**总开关**（DESIGN.md §1.2 机制 2）。
    // 关掉时：只保留重力（+阻尼），与 M2 行为一致——供单测隔离重力、以及
    // 用户对照“有力场 vs 无力场”。默认开。
    // 注意：关掉只是跳过力场累加，**不重建**关联表（开关瞬时生效）。
    bool spring_enabled() const { return spring_enabled_; }
    void SetSpringEnabled(bool enabled) { spring_enabled_ = enabled; }

    // 重力加速度 g（m/s²），方向恒为 -Y；可为负（负 = 反重力 / 上浮，供试验）。滑条范围
    // [kMinGravity, kMaxGravity]。查询与设置都走这里；设置时只 CHECK 有限，不夹断。
    float gravity() const { return gravity_; }
    // Pre-condition: gravity 有限（可为负）；非有限值崩。
    void SetGravity(float gravity);

    // 地面高度的 y（米）。地面为水平面（法线 +Y），低于它的顶点被**纯位置投影**回
    // 地面（不注入动能，见 .cc）。查询与设置都走这里。
    float ground_y() const { return ground_y_; }
    // 设置地面高度。CHECK 有限；无值域限制（地面可以在任意高度）。
    void SetGroundY(float ground_y);

    // ── 全局速度上限（数值鲁棒性）──
    //
    // 每子步末把速度矢量的模长截到 v_max（m/s）；**0 = 不限（默认）**。
    // 动机（2026-10-01 Danis）：低 M_total ⇒ 每点质量 m 极小 ⇒ 同样的力给出巨大加速度
    // （a = F/m）⇒ 固定子步下显式积分失稳（顶点被甩飞、拉出“淌”状长条）。限速是全局兜底。
    float max_speed() const { return max_speed_; }
    // Pre-condition（不满足即 LOG(FATAL)）：v_max 有限且 >= 0。
    void SetMaxSpeed(float v_max);

    // ── 人体排斥（可选；见 james_pm/glb_repulsion.txt 的设计）──
    //
    // 语义：每子步末（地面投影之后）对每个仿真点：查「最近的人体三角形」，若在**体内**
    //   或离表面 < buffer，则把它**投影**到「最近点 + buffer·外向法线」（= 离开体表
    //   buffer 距离）；并把指向体内的法向速度分量清零（同地面投影：纯位置投影、不注入动能）。
    // 用途：让衣服被人体“撑开”，不嵌进身体；buffer 防衣服彻底贴合（0 = 贴着表面）。
    //
    // 法线方向：判定“体内/体外”用最近三角形的外法线（点在外法线负侧=体内）。⇒ **前提：
    //   人体网格法线朝外且几何相对光滑**（与本工具使用的人体资产一致）。
    //
    // 命中范围：受传入 matcher 的 local_distance 限制（本工具 = 5cm）；离体表更远的点
    //   不会被排斥（靠衣物整体拉扯解决）——故 buffer 设到 >5cm 时效果会被 5cm 查询半径截断。
    //
    // matcher：外部拥有，必须比本对象**活得久**（本类只借用，不拥有；传 nullptr = 关查询）。
    void SetBodyMatcher(const geom::TriangleMatcher3d<double>* matcher);
    bool body_repulsion_enabled() const { return body_repulsion_enabled_; }
    void SetBodyRepulsionEnabled(bool enabled) { body_repulsion_enabled_ = enabled; }
    float body_buffer() const { return body_buffer_; }
    // Pre-condition（不满足即 LOG(FATAL)）：buffer 有限且 ∈ [kMinBodyBuffer, kMaxBodyBuffer]。
    void SetBodyBuffer(float buffer);

    // ── 切向（平行于表面）速度保留系数（Danis 2026-10-02）──
    //
    // clamp 时：**消除沿法向的速度分量**；平行（切向）分量乘本系数：
    //   v_new = parallel_factor · (v − (v·n)n)。
    // 0 = 切向也全消（完全粘住）；1 = 全保留（默认，即原行为）。
    static constexpr float kMinBodyParallelDamping = 0.0f;
    static constexpr float kMaxBodyParallelDamping = 1.0f;
    static constexpr float kDefaultBodyParallelDamping = 1.0f;
    float body_parallel_damping() const { return body_parallel_damping_; }
    // Pre-condition（不满足即 LOG(FATAL)）：factor 有限且 ∈ [0,1]。
    void SetBodyParallelDamping(float factor);

    // ── 即时操作：对**当前仿真状态**就地施加变换（面板 / 交互驱动）──
    //
    // 用途（Danis 2026-10-01）：穿衣工具的左上面板要在**仿真进行中**也能改变这件衣服的
    // 位置 / 朝向 / 大小，且**不中断**仿真（不是重建）。语义：
    //   - 位置与“绑定姿态”（力的参考形状 pij(0)）**同步变换**。必须同步，否则弹簧会把
    //     顶点拽回旧形状：旋转 45° 会给出 ~10⁴ m/s² 量级的恢复力（直接炸）；同步后是
    //     **刚体式重新摆位**，仿真从中继续。
    //   - **速度**：只有**旋转**会让速度跟着转；平移 / 缩放**不动速度**。
    //
    // 这些操作只改内部状态（位置 / 速度 / 绑定姿态），不改拓扑 / 关联表；调用后
    // mesh() 的**位置**立即更新（**法线 / 切线不随之更新**——由显示层自行重算）。

    // 平移：位置与绑定姿态 += delta；速度不变。
    // Pre-condition（不满足即 LOG(FATAL)）：已 Init；delta 各分量有限。
    void ApplyTranslation(const geom::Vec3<float>& delta);

    // 绕轴 axis、通过 pivot 的直线旋转 degrees 度（右手系逆时针）：位置与绑定姿态
    // 绕 pivot 旋转，**速度也旋转**（旋转 ⇒ 角速度）。
    // Pre-condition（不满足即 LOG(FATAL)）：已 Init；degrees / pivot 有限。
    void ApplyRotation(Axis axis, float degrees, const geom::Vec3<float>& pivot);

    // 以 pivot 为中心等比缩放 factor 倍：位置与绑定姿态缩放；**速度不变**。
    // Pre-condition（不满足即 LOG(FATAL)）：已 Init；factor > 0 且有限；pivot 有限。
    void ApplyScaling(float factor, const geom::Vec3<float>& pivot);

    // ── 仿真点速度（纯查询）──
    //
    // 与 sim_positions() 同序同长：索引 0..original_point_count()-1 = 原始顶点，
    // 其后为虚拟顶点。未 Step 过时全为 0。供单测/调试读取。
    const std::vector<geom::Vec3<float>>& sim_velocities() const {
        return sim_velocities_;
    }

    // 当前取景包围盒（纯查询）。
    SimBounds Bounds() const;

    // 回到**启动几何**（Init 时的网格，含长边虚拟点 / 连通性桥接点）并清零时间 / 步数 / 速度。
    // ⭐ 也会把**物理绑定姿态**（bind_positions_ / nb_init_dist_）一并复位回启动几何——
    //   因为 ApplyTranslation/Rotation/Scaling 会改绑定姿态；若不复位，Reset 只能回到“变换后
    //   的姿态”，等于没重置（2026-10-02 Danis 报的 bug）。故内部用**独立**的 startup_positions_
    //   快照（Apply* 从不改它），而不是 bind_positions_。
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

    // 连通性修复（Danis 2026-10-02）：保证关联图是**单一连通分量**。
    // 衣服网格在关联图上可能裂成多个子网 → 子网只受重力、独自坠落。两种成因分别处理：
    //   ① 空间间隙 > d：取最大分量为“主网”，对每个非主分量找它与主网的**最近点对**，
    //      沿线插**桥接虚拟点**（间距 ≤ 0.95 d），再重建关联表（迭代）；
    //   ② 间隙 ≤ d 但被 kMaxNeighbors 顶 k 截断砍掉了跨网边（实测战术背心就是这种）：
    //      直接**强制补一条对称边**（不重建）。
    // 桥接点**追加在末尾**（index ≥ vertex_count_ ⇒ 属“虚拟点”，不进输出 mesh），
    // 故本函数**必须在任何按点数分配速度/缓存缓冲之前**调用。
    // Pre-condition: d > 0；neighbors_ 已构建。
    void EnsureNeighborGraphConnected(float d);

    // 并查集求关联图的连通分量：返回每个点的分量标签（连续 0..C-1，C=分量数）；
    // 空图返回空 vector。neighbor_component_count() 与 EnsureNeighborGraphConnected 共用。
    std::vector<uint32_t> ComputeComponentLabels() const;

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

    // 人体接触的**几何外侧方向**缓存（与 sim_positions_ 同长）：上一子步查到的人体
    // 外方向（单位向量）；(0,0,0) = 上一子步无接触。第一趟拿它做**预速度约束**（把
    // 指向体内的 v_half 分量去掉），使 drift **不会**把接触点往体内飘——这是消除
    // “硬投影×硬弹簧 ⇒ 每子步位置锯齿（阻尼无效）”的关键（见 IntegrateSubstep 注释）。
    // mutable：ApplyBodyRepulsion 是**逻辑 const**（纯查询 + 位置投影，不改可观测状态），
    //   但需要把本子步查到的接触方向写进这份**跨步缓存**，故声明为 mutable。
    mutable std::vector<geom::Vec3<float>> body_outward_buf_;

    // 当前重力加速度（m/s²，可负），方向 -Y。可由 SetGravity 覆盖。
    float gravity_ = kDefaultGravity;

    // 总质量 M_total（kg）与力系数 F（N）。见 .h 顶部常量说明。
    float total_mass_ = kDefaultTotalMass;
    float force_coeff_ = kDefaultForceCoeff;
    // 速度指数衰减系数 k（1/s）。默认 kVelocityDamping（0.1），可由滑条覆盖。
    float velocity_damping_ = kVelocityDamping;
    // 弹簧力场总开关（默认开）；见 SetSpringEnabled。
    bool spring_enabled_ = true;

    // ── 关联邻居表（DESIGN.md §2.1；Init 时一次性建立，之后永不更新）──
    // neighbors_[i] = 与点 i 初始距离 <= d 的点的下标列表（不含 i 自身）。
    std::vector<std::vector<uint32_t>> neighbors_;
    // nb_init_dist_[i][k] = |pij(0)| = 点 i 与其第 k 个关联点 j = neighbors_[i][k]
    // 的**初始**距离（力的公式 §2.3 的分母用）。与 neighbors_ 同构同序。
    // 注：“初始”指“当前绑定姿态下”（Init 时建立；之后被 ApplyScaling 随缩放一起缩放——
    //     因为它是绑定的 offset 的模长，理应随绑定姿态缩放；旋转 / 平移不改变模长）。
    std::vector<std::vector<float>> nb_init_dist_;

    // 仿真点集合的**绑定姿态位置**（与 sim_positions_ 同序同长，含虚拟顶点）。
    // 力的公式里有 pij(0) = v_j(0) - v_i(0)（§2.2），需要绑定姿态坐标；
    // 而 sim_positions_ 会被 Step 推着走，bind_mesh_ 又不含虚拟顶点，
    // 故单独快照一份（Init/Reset 时更新）。
    // ⚠️ 注意：ApplyTranslation/Rotation/Scaling 会**同步变换 bind_positions_**（否则弹簧
    //   会把顶点拽回旧形状），故它反映的是“**当前变换后**的静止形状”，**不是**启动几何。
    std::vector<geom::Vec3<float>> bind_positions_;

    // 启动几何的**原始快照**（Init 时录入，Apply* **绝不**改它）。
    // 专供 Reset()：重置必须回到“clothing tool 启动时的样子”，而 bind_positions_ 已被
    //   即时变换污染（见上），不能直接用它（否则重置只回到“变换后的姿态”，等于没重置）。
    std::vector<geom::Vec3<float>> startup_positions_;

    // 当前地面高度 y（米）。低于它的顶点被投影回地面（水平面，法线 +Y）。
    float ground_y_ = kDefaultGroundY;

    // 全局速度上限（m/s，0 = 不限）。见 max_speed()。
    float max_speed_ = 0.0f;

    // 人体排斥（见 SetBodyMatcher / SetBodyBuffer / SetBodyRepulsionEnabled）。
    const geom::TriangleMatcher3d<double>* body_matcher_ = nullptr;  // 借用（不拥有）
    bool body_repulsion_enabled_ = false;
    float body_buffer_ = kDefaultBodyBuffer;
    // 切向速度保留系数（0~1；见 SetBodyParallelDamping）。
    float body_parallel_damping_ = kDefaultBodyParallelDamping;

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

    // 全局速度上限：把每个点的速度截到 max_speed_（>0 时）。子步末调用（见 .cc）。
    void ClampMaxSpeed();

    // 对单点做人体排斥（改 x / v），并把接触外方向写回 body_outward_buf_[idx] 供下一
    // 子步第一趟的预速度约束。
    //   x_pre = 本子步 drift **前**的位置 x(t)（用于二分求“本子步新穿入体表”的逃逸点）。
    // 算法（Danis 2026-10-02）：
    //   - 用**带 buffer 的体内判定** IsInsideBodyWithBuffer 判“在体内(含 buffer 壳)”；
    //   - 若 x(t) **也在**体内 → 旧方式：投影到 最近点 + buffer·外向几何方向；
    //   - 若 x(t) 在体外（本子步新穿入）→ 在线段 [x(t), x(t+dt)] 上**二分 10 轮**，求
    //     离 x(t+dt) 最近的“不在体内”点（≈ 穿越点）；
    //   - clamp 点法线 = 它**最近邻三角形的朝外法线**；
    //   - 速度：消除沿 n 的分量，平行分量乘 body_parallel_damping_。
    // 无 matcher / 开关关 / 查询未命中则不动（缓存清零）。
    // Pre-condition: x/v != nullptr；idx < 仿真点数。
    void ApplyBodyRepulsion(size_t idx, const geom::Vec3<float>& x_pre,
                            geom::Vec3<float>* x /*inout*/,
                            geom::Vec3<float>* v /*inout*/) const;

    // ── 人体最近三角形查询（内部工具）──
    //
    // BodyHit：点 p 的最近人体三角形查询结果。ok=false = 未命中（无 matcher 或逃出
    //   查询半径 local_distance）。normal 是最近三角形的**朝外**法线（假定资产绕序朝外）；
    //   signed_dist = (p − closest)·normal：>0 体外 / <0 体内。
    struct BodyHit {
        bool ok = false;
        geom::Vec3<double> closest = geom::Vec3<double>(0.0, 0.0, 0.0);
        geom::Vec3<double> normal = geom::Vec3<double>(0.0, 0.0, 0.0);
        double signed_dist = 0.0;
    };
    // 查 p 的最近人体三角形（借 body_matcher_）。
    BodyHit QueryBody(const geom::Vec3<double>& p) const;
    // 「带 buffer 的体内」判定：命中且 signed_dist < body_buffer_（buffer=0 即严格体内）。
    bool IsInsideBodyWithBuffer(const geom::Vec3<double>& p) const;
    // 在 [outside, inside] 上二分 rounds 轮，返回**离 inside 最近**的“不在体内(带 buffer)”点。
    // Pre-condition: inside 在体内；outside 通常在外（若也在内，结果退化为 outside）。
    geom::Vec3<double> BisectEscapeBoundary(const geom::Vec3<double>& outside,
                                            const geom::Vec3<double>& inside,
                                            int rounds) const;

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
