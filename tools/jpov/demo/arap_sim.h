// JPOV ARAP 软体仿真 —— 「最简力学模型」（需求方 Danis 2026-09-23 指定）
//
// 质点质量全部 m = 1，F = m·a，一共**只有四个力**（没有别的）：
//
//   ① 胡克弹簧：每条网格边一根（三角形的三条边都要），
//          F = −k_e·(L − L0)·û ,   k_e = hooke / L0
//      **按初始边长归一化**：边越长刚度越小。理由（"弹簧串联"）——把一条长边 L
//      细分成 n 段（每段原长 L0 = L/n，k_e = n·hooke/L），n 根串联的等效刚度
//          k_串 = k_e / n = hooke / L
//      与 L 无关、也与"分了几段"无关（分辨率无关）。L = L0（初始长度）处力为零，
//      即**原长就是力的零点**。
//   ② 重力 g。
//   ③ ARAP 局部形状恢复力：F_i = b·(goal_i − x_i)，
//          goal_i = c_i + R_i·(rest_i − rest_c_i)
//      meshless shape matching（Müller et al. 2005），与 Sorkine & Alexa 2007 的
//      ARAP 同源。**不是"拉回原始世界坐标"**：目标是局部的（c_i / rest_c_i = 当前 /
//      原始 1-ring 质心，R_i = 邻域最优旋转），整个物体可任意平移旋转而代价为零，
//      只有局部形状被拉伸/压扁才付力。R_i 由协方差 A = Σ (x_j−c_i)⊗(rest_j−rest_c_i)
//      的极分解（Higham 迭代 R ← ½(R+R⁻ᵀ)）得到。
//   ④ 线性阻尼：F_i = −u·v_i。
//
// 积分：显式（半隐式）欧拉，固定步长（需求方指定 0.05 s）：
//      v ← v + (F/m)·h ,   x ← x + v·h        （m = 1）
//
// 地面：水平面 y = ground_y，**位置硬约束 + 法向速度归零**（完全非弹性）。
//   这是边界条件、不是力，所以它不带"刚度"参数；四参数模型里也没有切向摩擦。
//
// ═══ 数值稳定性（诚实说明，靠"分析"而不是靠"多加几个力"掩盖）═══
//   显式积分有稳定上限 h < 2/ω_max（ω_max = 最高本征频率）。见 max_stable_dt()：
//   用 Gershgorin 行和给出 ω²_max 的上界估计。注意 k_e = hooke/L0 ⇒ **越短的边越硬**，
//   故稳定性由网格里**最短的边**决定：同一个 hooke，细网格（边长短）需要更小的步长。
//   若要更小的步长，用 substeps（把 dt 均分）——同一格式、只改分辨率，不改力的模型。
//
// ═══ 力的计算是状态的纯函数 ═══
//   ComputeForcesAt(positions, velocities, out) 只读、不改状态。Step 内部就是
//   "算力 → 积分 → 地面"。把力律独立出来是为了它**能直接被单测**（在已知构型上
//   比对解析力），而不是只能"跑一步再看位移"间接推断。
//
// ═══ 拓扑：物理用【焊接】拓扑，渲染用【不焊接】拓扑（两者必须分开）═══
//   物理侧按位置把重合顶点（ε = 1e-6）焊成一个质点：UV 缝两侧的顶点几何上是同一个
//   表面点，若各自独立受力会被弹簧力撕开成裂缝。渲染侧（法线/切线）**不焊接**——
//   顶点分裂是数据的一部分，硬边靠它保留（见 mesh_geometry.h 的长注释）。
//   故本类对外只暴露「逐顶点位置写回」接口，不与渲染侧共享拓扑。
//
// ═══ 多 primitive = 同一个物理物体 ═══
//   资产常按材质把同一个物体切成多个 primitive（实测 lantern.glb = LanternPole_Body /
//   _Chain / _Lantern 三个）。**它们应当被当作同一个物体仿真**：焊接跨 primitive 进行，
//   接缝处位置重合的顶点会被焊成同一质点，故各部件互相支撑而不会各自飞散。
//
// 量纲：位置 = 模型局部坐标；时间 = 秒；重力 = 模型长度/秒²（默认 9.8）。
// 确定性：不做任何随机采样，同样的输入 / dt 序列必然得到同样的输出（便于出 gold）。

// ═══ 物理约定（需求方 Danis 2026-09-23 定稿；MKS；单位铁律：长度 = 米）═══
//   glb 的 cm 等资产在**装载边界**统一换算成米，此后仿真内部一切量都是 MKS。
//
//   ── 三个材料常数（都从材料手册查得到）──
//     ρ  = area_density_kg_per_m2     面密度 [kg/m²]      = 体密度 × 厚度
//     c  = spring_stiffness_per_area  胡克材料常数 [N/m³] = 杨氏模量 E × 厚度
//     c' = arap_stiffness_per_area    ARAP 材料常数 [N/m³] = 剪切模量 G × 厚度
//
//   ── 三个逐量表达式（**都与顶点数 N 同阶反比** ⇒ 物理效果不随网格密度漂移）──
//         a_i  = Σ_{t∈tris(i)} area(t)/3          （该质点摊到的面积，Σa_i = 总面积 A）
//         m_i  = ρ · a_i                          [kg]      ∝ 1/N
//         k_e  = c · √(a_i·a_j)                   [N/m]     ∝ 1/N   （边 (i,j)）
//         β_i  = c' · a_i                         [N/m]     ∝ 1/N
//
//   🔑 为什么必须这么定（需求方 2026-09-23 一起推的结论）：
//     · 三者幂次一致 ⇒ k_e/m = c/ρ、β/m = c'/ρ **与 N、与尺度 L0 都无关** ⇒
//       同一物体换网格密度、或不同尺度的同种材料，行为一致（这才是"不漂"的含义）。
//     · 旧的 `k_e = T/L0` 让 k_e ∝ N^0.5（越密越硬）、`β = 常数` 让 β ∝ N⁰，
//       两者与 m ∝ 1/N 幂次失配 ⇒ 细网格必然"又轻又硬"（实测抛石机 k_e/m 达 5e11，
//       稳定步长 1.5e-6 s，必炸）。
//     · 取几何平均 √(a_i·a_j) 是为了非均匀网格（腋下密、平面疏）时两边面积不等也自洽。
//
//   ── 阻尼（与质量无关，需求方 2026-09-23 指定）──
//         a_damp = −u · v_i                       u [1/s]，默认 0.1
//     写成加速度（而不是力）⇒ 每个质点的速度衰减时间常数恒为 1/u，与质量/网格无关。
//     阻尼只影响运动过程，不改稳态（稳态由 弹簧/ARAP/重力 三者决定）。
//
//   ── 默认材质 = 厚度 1 cm 的橡胶皮（需求方选定，用来验收）──
//     ρ = 11   kg/m²   （1100 kg/m³ × 0.01 m）
//     c = 1.5e4 N/m³   （E ≈ 1.5 MPa × 0.01 m）
//     c'= 5e3  N/m³    （G ≈ 0.5 MPa × 0.01 m，橡胶 G ≈ E/3）
//     ⇒ 60 Hz（h = 1/60 s）显式积分稳定：逐质点 row = n·c/ρ + β/m ≈ 6.3·c/ρ ≈ 8.6e3
//       < (2/h)² = 1.44e4，余量约 1.65 倍。（n = 平均 1-ring 度 ≈ 6）
//     ⚠️ 通用判据：稳定要求 row ≤ (2/h)²，即 c ≤ 2290·ρ（h = 1/60 s 时）。
//        比橡胶更硬的材料（E 更大）或更薄的面密度会超 ⇒ 只能降 ρ 或换隐式积分。

#ifndef JPOV_DEMO_ARAP_SIM_H_
#define JPOV_DEMO_ARAP_SIM_H_

#include <array>
#include <cstdint>
#include <vector>

#include "tools/jpov/interface/mesh.h"

namespace jpov_arap {

// 焊接判定阈值（模型局部坐标）。只用于【物理】拓扑的建立；
// 仅在构建时（bind pose）判定一次，之后永久固定——变形后位置会漂移，
// 若每帧重新焊接会把「本已分开的两点」错焊。
inline constexpr float kWeldEpsilon = 1e-6f;

// 仿真参数：材质与物理量（MKS；长度单位 = 米，见文件头）。
struct ArapSimConfig {
    // ── 材质常数 ①：面密度（kg/m²）── 顶点质量 m_i = ρ·a_i
    float area_density_kg_per_m2 = 11.0f;

    // ── 材质常数 ②：胡克材料常数（N/m³）── 边弹簧 k_e = c·√(a_i·a_j)
    //   物理含义 = 杨氏模量 × 厚度。1 cm 橡胶皮 ≈ 1.5e4。
    float spring_stiffness_per_area = 1.5e4f;

    // ── 材质常数 ③：ARAP 材料常数（N/m³）── 形状力 β_i = c'·a_i
    //   物理含义 = 剪切模量 × 厚度。橡胶 G ≈ E/3 ⇒ 1 cm 橡胶皮 ≈ 5e3。
    float arap_stiffness_per_area = 5.0e3f;

    // ── 重力加速度（m/s²）── 默认 1.0（需求方指定）。
    float gravity_magnitude = 1.0f;

    // ── 阻尼（1/s，**与质量无关**）：a_damp = −u·v ⇒ 速度衰减时间常数 1/u ──
    float damping_per_second = 0.1f;

    // 步长细分：Step(dt) 内部把 dt 均分为 substeps 个子步。
    // 默认 1；需求方 2026-09-23 明确：**最多接受 60 Hz（h ≥ 1/60 s）**，
    // 再细分不可接受 ⇒ 材质参数必须自己满足稳定条件（见文件头的通用判据）。
    int substeps = 1;

    // 地面平面 y（世界坐标，米）与其上方留出的间隙。
    // 碰撞 = 位置硬约束 + 法向速度归零（完全非弹性）。模型里**没有摩擦**（需求方决定）。
    float ground_y = -3.0f;
    float ground_offset = 0.002f;
    bool enable_ground = true;
};

// 质量与稳定性的前置诊断（**不建仿真器**就能算）：用于在 Build 之前就告诉用户
// 「这份资产按当前面密度会有多重、按当前胡克系数会不会超过稳定步长」。
//
// 为什么需要它：这份仿真是**显式积分**，而 k_e/m = (T/L0)/m 完全由资产决定
// （L0 是网格边长、m 是顶点面积×面密度）。高模资产（4k+ 顶点、边长 0.02 m）在
// T=100 N/m 下 k_e/m 可到 1e5 量级 ⇒ 稳定步长 ~1e-3 s，而需求方指定步长 0.05 s，
// **必然发散**。这是资产/材质的物理事实，不是调参能绕的；早点说清楚比看模型飞走好。
struct MassDiagnostics {
    float total_area_m2 = 0.0f;
    float total_mass_kg = 0.0f;
    // 逐边收集 max( (1/L0)/m_i, (1/L0)/m_j )：它乘 T 就是 k_e/m 的保守上界。
    float max_edge_k_over_m = 0.0f;
};

// 从一组 CPU 网格算 MassDiagnostics。
//
// Pre-condition:  meshes 非空；各网格 positions/indices 合法（同 ArapSim::Build）。
// Post-condition: 返回面积/质量与 max_edge_k_over_m（无法计算时为 0）。
MassDiagnostics AnalyzeMasses(const std::vector<jpov::MeshData>& meshes,
                              float area_density_kg_per_m2);

// 同上，但面密度取自 config。
MassDiagnostics AnalyzeMasses(const std::vector<jpov::MeshData>& meshes,
                              const ArapSimConfig& config);

// 一个可仿真的软体：由若干 CPU 网格（glTF 的 primitive）构建，内部维护质点位置/速度。
//
// 生命周期：Build() → 反复 Step() → （可选）Reset() 回到 bind pose 再跑。
class ArapSim {
public:
    ArapSim() = default;

    // 从 CPU 网格构建仿真（建立焊接映射、边表、邻域、面密度质量、原始形状目标）。
    //
    // Pre-condition:  mesh.positions 非空；mesh.indices 非空且为 3 的倍数、索引在界内。
    //                 config 的材质参数为 MKS（长度单位 = 米）。
    // Post-condition: 仿真就绪，当前状态 == bind pose（Reset() 的等价初始态）。
    void Build(const jpov::MeshData& mesh, const ArapSimConfig& config);

    // 从多个 CPU 网格构建【同一个物理物体】的仿真（跨 primitive 焊接，见文件头）。
    //
    // Pre-condition:  meshes 非空；每个 mesh 的 positions/indices 满足上一条约束。
    // Post-condition: 同单网格版；WriteBackPositions 按同样顺序写回各网格。
    void Build(const std::vector<jpov::MeshData>& meshes,
               const ArapSimConfig& config);

    // 恢复到 bind pose（清空速度）。对应界面上的「重置」按钮。
    void Reset();

    // 把【当前】位置/速度导出为快照 / 从快照恢复（诊断与"发散回滚"用）。
    //
    // 用途：显式积分在步长超限时会一步炸成 NaN；调用方在每步之前存一份快照，
    // 发现几何坏掉时用快照回滚，就能保证屏幕上不会出现"模型消失"。
    //
    // Pre-condition: 已 Build()；positions/velocities 长度 = particle_count()。
    void SnapshotState(std::vector<jpov::Vec3f>* positions /*output*/,
                       std::vector<jpov::Vec3f>* velocities /*output*/) const;
    void RestoreState(const std::vector<jpov::Vec3f>& positions,
                      const std::vector<jpov::Vec3f>& velocities);

    // 把当前状态整体绕 (pivot, axis) 旋转 angle_rad（刚体旋转 + 速度同步旋转）。
    //
    // 用途：① 交互上「把物体翻个面再摔」，比较不同入姿势下的行为；
    //       ② 单测里制造「整体旋转后形状力是否仍为零」的场景——若局部旋转 R_i
    //          写错（例如写成单位阵），绕任意轴转一下 ARAP 力就会爆开。
    //
    // Pre-condition: 已 Build()；axis 非零。
    void RotateCurrentState(const jpov::Vec3f& axis, float angle_rad,
                            const jpov::Vec3f& pivot);

    // 推进 dt 秒（内部按 substeps 切分；子步格式 = 半隐式欧拉）。
    //
    // Pre-condition: 已 Build()；dt > 0；config_.substeps ≥ 1。
    void Step(float dt_seconds);

    // 更新参数（不重置状态；仅影响后续 Step）。
    void SetConfig(const ArapSimConfig& config);
    const ArapSimConfig& config() const { return config_; }

    // 在【给定状态】下计算逐质点合力（只读；不改任何内部状态）。
    //
    // 这是全部力律的唯一实现：Step 内部就是调用它。独立暴露是为了让力律**可直接
    // 单测**（在已知构型上比对解析力），而不是只能通过"跑一步看位移"间接观测。
    //
    // 返回的是 F/m（即加速度），m 由面密度给出（见文件头）。
    //
    // Pre-condition: 已 Build()；positions/velocities 长度 == particle_count()。
    // Post-condition: *out 长度 == particle_count()。
    void ComputeForcesAt(const std::vector<jpov::Vec3f>& positions,
                         const std::vector<jpov::Vec3f>& velocities,
                         std::vector<jpov::Vec3f>* out /*output*/) const;

    // ── 只读查询 ──
    size_t particle_count() const { return particle_count_; }   // 焊接后质点数
    size_t vertex_count() const { return vertex_positions_.size(); }  // 全部网格顶点数
    size_t mesh_count() const { return mesh_vertex_begin_.size() - 1; }

    // 当前逐顶点位置（未焊接：同一位置的多个副本共享质点位置）。
    const std::vector<jpov::Vec3f>& vertex_positions() const {
        return vertex_positions_;
    }
    // 当前逐质点速度（诊断/单测读速度用，阻尼律是 F = −u·m·v）。
    const std::vector<jpov::Vec3f>& velocities() const { return vel_; }

    // 逐质点质量（kg）、逐质点面积（m²）与"总质量 / 总表面积"诊断量。
    // areas() 是材质定标的基准（k_e ∝ √(a_i a_j)、β ∝ a、m ∝ a），单测要靠它算解析值。
    const std::vector<float>& masses() const { return mass_; }
    const std::vector<float>& areas() const { return particle_area_m2_; }
    float total_mass() const { return total_mass_; }
    float surface_area() const { return surface_area_; }

    // 把当前逐顶点位置写回网格（不触碰 UV / flags）。
    //
    // Pre-condition: mesh->positions.size() 与 Build 时对应网格的顶点数一致。
    void WriteBackPositions(jpov::MeshData* mesh) const;
    void WriteBackPositions(std::vector<jpov::MeshData>* meshes) const;

    // 诊断量：ARAP 形状力目标的 RMS 偏差 sqrt(mean |goal_i − x_i|²)（模型长度单位）。
    // 0 = 每个质点的局部邻域都严格是「原始形状的旋转」（形状完全保持）。
    // ⚠️ 语义：这是【局部形状】偏差，不是"被压扁了多少"——整体被压成饼时局部旋转
    // 会跟着改变，该量仍可能接近 0（要看压扁程度请用 edge_distortion_rms()）。
    float shape_residual_rms() const;

    // 诊断量：边长畸变 RMS（旋转/平移不变）
    //   sqrt(mean over edges of (当前边长/原始边长 − 1)²)
    // 0 = 完全未拉伸；越大 = 被拉长/压扁越厉害。这是「物体当前有多软」的直观度量。
    float edge_distortion_rms() const;

    // 诊断量：最近一次计算局部旋转时，因邻域退化（近似共面/共线 —— 薄壳、单层贴面）
    // 而【无法解出唯一旋转】、退回单位阵的质点个数。
    //
    // 为什么要有这个量：R_i 靠 1-ring 协方差的极分解得到，而**薄片结构（灯罩玻璃、
    // 单层纸面、布料）的 1-ring 本身就近似共面** ⇒ 协方差秩亏 ⇒ 3D 极分解退化。
    // 本实现用「法向增广项」（见 ComputeLocalRotationsInto）把秩补满；该计数用来
    // 验证该手段是否生效（应为 0 或极少）。
    size_t degenerate_rotation_count() const {
        return degenerate_rotation_count_;
    }

    // 显式积分的稳定步长上限估计（秒）：h < 2/ω_max。
    //
    // ω²_max 用 Gershgorin 行和上界估计：
    //   row_i = Σ_{边 (i,j)} k_e/m_i + β_i·n_i/((n_i+1)·m_i)
    // 代入本模型的三条表达式（k_e = c√(a_i a_j)、β = c'·a、m = ρ·a）后等价于
    //   row_i ≈ n_i·c/ρ + c'/ρ·n_i/(n_i+1)      ← **与网格密度、与尺度都无关**
    // 故默认材质（c=1.5e4, c'=5e3, ρ=11, n≈6）给出的上限 ~0.022 s > 1/60 s ⇒ 60 Hz 稳。
    // 返回 2/sqrt(max_i row_i)。**只是估计**（真实本征值 ≤ 该行和），用于提示步长。
    //
    // Pre-condition: 已 Build()。
    float max_stable_dt() const;

    // 被短边过滤跳过的边数（见 .cc 里 min_edge_length_ 的说明）——数据卫生指标。
    size_t skipped_short_edge_count() const { return skipped_short_edges_; }

    // 约束图规模（诊断/性能评估用）。
    size_t edge_count() const { return edges_.size(); }

private:
    // 建立焊接映射（位置 ε 内合并，跨全部输入网格）、边表、邻域、原始形状目标。
    void BuildTopology(const std::vector<jpov::MeshData>& meshes);

    // 用给定位置算每个质点的邻域最优旋转 R_i，写 R_i·(rest_i − rest_c_i) 到 *out。
    // 独立成「读给定位置」的形态：Step 与 ComputeForcesAt 都用同一份实现（不分叉）。
    void ComputeLocalRotationsInto(const std::vector<jpov::Vec3f>& pos,
                                   std::vector<jpov::Vec3f>* out /*output*/) const;

    // 质点 i 在给定位置序列下的面积加权法线（用关联三角形算）。
    jpov::Vec3f ParticleNormal(uint32_t i,
                               const std::vector<jpov::Vec3f>& pos) const;

    // 质点 i 在给定位置序列下的 1-ring 邻域质心（含自身）。
    jpov::Vec3f NeighborhoodCentroid(const std::vector<jpov::Vec3f>& pos,
                                     uint32_t i) const;

    // 地面位置约束 + 法向速度归零（完全非弹性）。就地修改 pos_ / vel_。
    void ProjectGround();

    // 把质点位置散布到逐顶点数组 vertex_positions_（未焊接写回）。
    void ScatterToVertices();

    ArapSimConfig config_;

    // ── 焊接映射（构建时定，之后不变）──
    size_t particle_count_ = 0;
    std::vector<uint32_t> particle_of_vertex_;   // 全局顶点 → 质点
    std::vector<size_t> mesh_vertex_begin_;      // 各网格在全局顶点空间的前缀和（长度 +1）

    // ── 质点状态（焊接拓扑上的物理量；长度 = 米、质量 = kg）──
    std::vector<jpov::Vec3f> rest_pos_;   // bind pose 质点位置（原始形状目标）
    std::vector<jpov::Vec3f> pos_;        // 当前质点位置
    std::vector<jpov::Vec3f> vel_;        // 质点速度
    std::vector<jpov::Vec3f> force_;      // 逐质点合力（Step 的暂存缓冲，避免每步分配）
    std::vector<float> mass_;             // 逐质点质量（kg，由面密度 × 顶点面积得到）
    std::vector<float> particle_area_m2_;           // 逐质点面积（m²，面密度的乘子）
    float spring_c_ = 0.0f;               // 本物体的胡克材料常数 c（N/m³）
    float arap_beta_c_ = 0.0f;            // 本物体的 ARAP 材料常数 c'（N/m³）
    float total_mass_ = 0.0f;             // Σm（kg）——诊断/验证面密度用
    float surface_area_ = 0.0f;           // Σ 三角形面积（m²）——同上

    // ── 约束图 ──
    struct Edge {
        uint32_t a;
        uint32_t b;
        float rest_length;
        // √(a_a·a_b)：两端质点面积的几何平均。胡克材料常数 c 乘它就是边刚度
        // k_e = c·√(a_i·a_j)（面积定标，见文件头）。**在拓扑建立时算一次**（面积是
        // rest 属性，不随变形改变），避免每子步开方。
        float geom_mean_area;
    };
    std::vector<Edge> edges_;
    std::vector<std::vector<uint32_t>> ring_;   // 质点的 1-ring 邻域（不含自身）

    // ── 形状匹配（ARAP）：原始目标、薄壳法向增广 ──
    std::vector<jpov::Vec3f> rest_local_centroid_;   // rest_c_i（含自身的邻域质心）
    std::vector<std::array<uint32_t, 3>> tris_;      // 三角形（质点级）
    std::vector<std::vector<uint32_t>> particle_tris_;  // 质点 → 关联三角形下标
    std::vector<jpov::Vec3f> rest_normal_;           // 质点 rest 面积加权法线
    std::vector<float> ring_scale_sqr_;              // 邻域尺度 δ²（法向增广项权重）

    // 退化旋转计数（诊断用；mutable 因为 const 的力/残差计算也会刷新它）。
    mutable size_t degenerate_rotation_count_ = 0;

    // 短边阈值（模型 rest 包围盒对角线的 1e-4 倍）与已跳过条数。
    //   ⚠️ 真实资产（尤其 Unity/Tripo 导出的多 primitive 模型）里常残留**极短的
    //   退化边**（长度 1e-5 量级）。而 k_e = T/L0 ⇒ 这类边刚度 ∝ 1/L0 极大，
    //   一步就能把它拉长千倍 ⇒ 直接毁掉整个仿真（实测路灯首帧畸变 230%、随后飞到
    //   15000）。故：拓扑（1-ring）保留这些边，但**弹簧力与畸变度量都跳过**它们。
    //   同样地，**三角形面积**也只在非退化的三角形上累加质量（否则零面积三角形
    //   不影响，但把退化面积算进总面积会让面密度失真诊断）。
    //   跳过条数会记录并在装载时打印（不静默）。
    float min_edge_length_ = 0.0f;
    size_t skipped_short_edges_ = 0;

    // ── 渲染侧位置副本（未焊接，每帧由质点位置散布而来）──
    std::vector<jpov::Vec3f> vertex_positions_;
};

}  // namespace jpov_arap

#endif  // JPOV_DEMO_ARAP_SIM_H_
