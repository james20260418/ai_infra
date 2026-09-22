// JPOV ARAP 软体仿真 — 把一件静态网格「穿上」（松弛式披挂 / relaxation draping）
//
// 目标：给定一个已经和人体【大致对齐】（公差在手臂半径以内）的衣物网格，让它在
//   「局部形状恢复力 + 碰撞 + 重力 + 阻尼」共同作用下自己演化到贴合状态。
//
// 力的构成（每个都可单独开关，便于做单变量消融实验）：
//   1. **局部形状恢复力（ARAP / meshless shape matching）**：每个顶点把自己
//      【1-ring 邻域】的形状拉回「原始形状经该邻域局部旋转后的样子」——
//         goal_i = c_i + R_i · (rest_i − rest_c_i)
//      其中 c_i / rest_c_i 是当前 / 原始邻域质心，R_i 是邻域的最优旋转（极分解）。
//      **注意这不是"全局拉回原始世界坐标"**：目标是局部的、平移+旋转不变的，
//      所以网格可以整体移动到任意位置/朝向，同时"记得自己原来长什么样"。
//      出处：Müller et al. 2005《Meshless Deformations Based on Shape Matching》，
//      与 Sorkine & Alexa 2007 的 ARAP 同源（都是"局部形状 → 只允许旋转"）。
//   2. **边长约束（stretch）**：普通胡克/距离约束，抵抗拉伸压缩。
//   3. **碰撞（ground / body）**：PBD 位置投影（**完全非弹性**，不是罚力）——
//      穿过的顶点直接推回到表面外。用户 2026-09-22 定稿采纳此法（无条件稳定，
//      且斥力系数与时间步解耦，避免"10 倍斥力"那类拍脑袋的罚刚度）。
//   4. **重力 + 速度阻尼 + 地面摩擦**：让系统从任意初始状态演化到稳态。
//
// ═══ 拓扑：物理用【焊接】拓扑，渲染用【不焊接】拓扑（两者必须分开）═══
//   物理侧按位置把重合顶点（ε = 1e-6）焊成一个质点：UV 缝两侧的顶点几何上是同一个
//   表面点，若各自独立受力会被约束力撕开成裂缝。渲染侧（法线/切线）**不焊接**——
//   顶点分裂是数据的一部分，硬边靠它保留（见 mesh_geometry.h 的长注释）。
//   故本类对外只暴露「逐顶点位置写回」接口，不与渲染侧共享拓扑。
//
// ═══ 多 primitive = 同一个物理物体 ═══
//   资产常按材质把同一个物体切成多个 primitive（实测 lantern.glb =
//   LanternPole_Body / _Chain / _Lantern 三个）。**它们应当被当作同一个物体仿真**：
//   焊接跨 primitive 进行，接缝处位置重合的顶点会被焊成同一质点，故各部件互相
//   支撑而不会各自飞散。（曾按 primitive 独立仿真，现象是「一部分立住、一部分
//   掉下去」，看起来像 bug —— 见 docs/jpov_arap_viewer_design.md §11。）
//   primitive 若不接触（例如一个场景里的两个分离物体），焊接自然不会连接它们，
//   行为退化为「两个独立物体」——这同样是正确的。
//
// ═══ 均匀顶点分布假设 ═══
//   本实现假设网格顶点分布均匀：所有质点质量相同（= 1）、形状匹配权重相同（= 1）。
//   非均匀网格（腋下密、平面疏）会让密处显得更硬。若日后发现局部硬化斑块，
//   再引入按顶点面积的权重归一化（本 PR 不做，避免过度设计）。
//
// 量纲：位置 = 模型局部坐标；时间 = 秒；重力 = 模型长度/秒²（默认 9.8）。
// 确定性：不做任何随机采样，同样的输入 / dt 序列必然得到同样的输出（便于出 gold）。

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

// 仿真参数。本 PR 全部在代码里配置（需求方要求：重力参数等先代码里配置）。
struct ArapSimConfig {
    // 重力加速度（模型长度 / 秒²）。默认 y-up 向下 9.8。
    jpov::Vec3f gravity = {0.0f, -9.8f, 0.0f};

    // 速度阻尼（1/秒）：每子步 vel *= exp(-velocity_damping_per_second · h)。
    // 0 = 无阻尼（会一直抖/震荡），越大越快静止。用"每秒衰减率"而非"每步乘子"，
    // 使阻尼与子步数无关（改 substeps 不会偷偷改变阻尼强度）。
    float velocity_damping_per_second = 1.0f;

    // ── 两个“恢复力”的强度（统一用【每秒恢复率】，单位 1/秒）──
    //
    // 物理量纲说明（踩过坑，很重要）：PBD 里“每子步投影比例 s”不是物理刚度——
    // 它等效于一根弹簧 k ≈ s/h²，而 h 很小（1/360 秒）⇒ s 取 0.1 看着小，实际等效
    // 刚度是巨量级，物体表现得像刚体，根本看不到重力压扁。因此这里改用
    // **每秒恢复率 rate**：
    //     每子步比例 s = 1 − exp(−rate · h)
    // 它才有直观含义：形状误差按时间常数 1/rate 衰减。
    //
    // 同时它决定「重力平衡下沉量」：静态下垂 ≈ g·h/rate。
    //   rate = 0.08 ⇒ 下垂 ≈ 0.34（比物体还大 ⇒ 长跑必瘫成一张纸，实测就是这个）
    //   rate = 3    ⇒ 下垂 ≈ 0.009（忽略不计，但碰撞瞬间的压缩仍看得见）
    // 故默认取 3.0（**曾误取 0.08，交互长跑时整个模型会“淌到地上”**）。
    float shape_restore_rate_per_second = 2000.0f;
    // 边长约束 [胡克/距离约束] 每秒恢复率。0 = 关闭。
    // 注意：ARAP 本身已包含“局部形状保持”，边长约束是额外的「不可拉伸」强度；
    // 两者同时取很大值会把物体变成完全刚体（连碰撞压扁都做不到）。
    float stretch_restore_rate_per_second = 2000.0f;
    // 弯曲约束 [二面角 / dihedral bending] 每秒恢复率。0 = 关闭。
    //
    // 🔑 为什么必须有它：**ARAP 允许「等距变形」（弯曲/折叠/旋转）零代价** ——
    //   它只抵抗拉伸，不抵抗折叠。于是薄板拼起来的物体（抛石机、灯罩、以及
    //   **布料**）会被重力像纸盒一样折平，而形状残差始终很小（实测抛石机落地后
    //   残差 0.008 却整体拍平 —— 纯等距折叠）。要“立得住/挺得起”就必须补弯曲刚度。
    //   实现用标准做法（布料仿真通用）：把「相邻两个三角形共享一条边时的
    //   **两个对顶点**」之间加一根距离约束 —— 它的静止长度就是 rest 时的对顶点距离，
    //   折叠会让该距离变小/变大从而被拉回，等效于二面角弹簧，但代价极低、易实现。
    float bend_restore_rate_per_second = 1500.0f;

    // 每帧子步数（渲染帧 dt 被均分成 substeps 个物理子步 → 更稳）。
    int substeps = 6;
    // 每个子步内的约束迭代次数（PBD Gauss-Seidel 扫描次数 → 越大约束越硬）。
    int solver_iterations = 4;

    // 地面摩擦（1/秒）：与地面【接触中】的质点，其切向（x/z）速度按
    //   v_t *= exp(-ground_friction_per_second · h)  衰减。
    // 0 = 无摩擦（冰面：落地后一路滑走，既不符合直觉也会让物体滑出镜头）；
    // 越大越“糙”。法向（y）分量不受影响——摩擦不产生法向力。
    float ground_friction_per_second = 6.0f;

    // 地面平面 y（世界坐标）与其上方留出的间隙：碰撞后约束 y ≥ ground_y + ground_offset。
    // ground_offset 给一点离地间隙，避免与地面 z-fighting（几何恰好贴合时来回闪）。
    float ground_y = -3.0f;
    float ground_offset = 0.002f;

    // 三个力的独立开关（单变量消融实验用：一次只关一个，观察差异归因于它）。
    bool enable_shape = true;
    bool enable_stretch = true;
    bool enable_bend = true;
    bool enable_ground = true;
};

// 一个可仿真的软体：由若干 CPU 网格（glTF 的 primitive）构建，内部维护质点位置/速度。
//
// 生命周期：Build() → 反复 Step() → （可选）Reset() 回到 bind pose 再跑。
class ArapSim {
public:
    ArapSim() = default;

    // 从 CPU 网格构建仿真（建立焊接映射、边表、邻域、原始形状目标）。
    //
    // Pre-condition:  mesh.positions 非空；mesh.indices 非空且为 3 的倍数、索引在界内。
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

    // 把当前状态整体绕 (pivot, axis) 旋转 angle_rad（刚体旋转 + 速度同步旋转）。
    //
    // 用途：① 交互上「把物体翻个面再摔」，比较不同入姿势下的回弹；
    //       ② 单测里制造「整体旋转后形状是否仍被保持」的场景——若形状恢复力
    //          不是旋转不变的（例如把局部旋转 R_i 写成单位阵），绕任意轴转一下
    //          就会让 shape_residual_rms() 爆开。
    //
    // Pre-condition: 已 Build()；axis 非零。
    void RotateCurrentState(const jpov::Vec3f& axis, float angle_rad,
                            const jpov::Vec3f& pivot);

    // 推进 dt 秒（内部按 substeps 切分）。
    //
    // Pre-condition: 已 Build()；dt > 0。
    void Step(float dt_seconds);

    // 更新参数（不重置状态；仅影响后续 Step）。ground_y 的变化下一帧生效。
    void SetConfig(const ArapSimConfig& config);
    const ArapSimConfig& config() const { return config_; }

    // ── 只读查询 ──
    size_t particle_count() const { return particle_count_; }   // 焊接后质点数
    size_t vertex_count() const { return vertex_positions_.size(); }  // 全部网格顶点数
    size_t mesh_count() const { return mesh_vertex_begin_.size() - 1; }

    // 当前逐顶点位置（未焊接：同一位置的多个副本共享质点位置）。
    const std::vector<jpov::Vec3f>& vertex_positions() const {
        return vertex_positions_;
    }

    // 把当前逐顶点位置写回网格（不触碰 UV / flags）。
    //
    // Pre-condition: mesh->positions.size() 与 Build 时对应网格的顶点数一致。
    void WriteBackPositions(jpov::MeshData* mesh) const;
    void WriteBackPositions(std::vector<jpov::MeshData>* meshes) const;

    // 诊断量：当前状态对形状约束的违反度 RMS（模型长度单位）。
    // 0 = 完全满足约束（每个质点都在自己的局部形状目标上）。
    // 注意语义：这是【约束违反度】，不是“被压扁了多少”——约束满足时它接近 0，
    // 即使整体已经被压成饼（因为压扁后的局部旋转已经跟着改变）。要看“压扁程度”
    // 请用 edge_distortion_rms()。
    float shape_residual_rms() const;

    // 诊断量：最近一次局部旋转计算中，因邻域退化（近似共面/共线 —— 薄壳、单层贴面）
    // 而【无法解出唯一旋转】、退回单位阵的质点个数。
    //
    // 为什么要有这个量：ARAP 的局部旋转 R_i 靠 1-ring 协方差的极分解得到，而
    // **薄片结构（灯罩玻璃、单层纸面、布料）的 1-ring 本身就近似共面** ⇒ 协方差
    // 秩亏 ⇒ 3D 极分解退化。本实现用「法向增广项」（见 ComputeLocalRotationsInto）
    // 把秩补满；这个计数用来验证该手段是否生效（应为 0 或极少）。
    size_t degenerate_rotation_count() const {
        return degenerate_rotation_count_;
    }

    // 诊断量：边长畸变 RMS（旋转/平移不变）
    //   sqrt(mean over edges of (当前边长/原始边长 − 1)²)
    // 0 = 完全未拉伸；越大 = 被拉长/压扁越厉害。这是“物体当前有多软”的直观度量，
    // 适合放在交互面板上观察回弹过程。
    float edge_distortion_rms() const;

    // 诊断量：弯曲畸变 RMS（"对顶点距离"相对 rest 的畸变，旋转/平移不变）。
    // 0 = 完全没被折叠；越大 = 局部折叠越厉害。配合 edge_distortion_rms() 可区分
    // 「被拉伸」与「被折叠」两种形变（ARAP 只挡前者，靠 bending 挡后者）。
    float bend_distortion_rms() const;

private:
    // 建立焊接映射（位置 ε 内合并，跨全部输入网格）、边表、邻域、原始形状目标。
    void BuildTopology(const std::vector<jpov::MeshData>& meshes);

    // 每个子步：按当前质点位置算每个质点的邻域最优旋转 R_i（极分解），
    // 把 R_i·(rest_i − rest_c_i) 写入 rot_。
    void ComputeLocalRotations();

    // ComputeLocalRotations 的通用形态：结果写入 *out（长度 = particle_count）。
    // 独立成 const 版本，供 shape_residual_rms() 现算一份「当前」旋转（不读 rot_ 缓存）。
    void ComputeLocalRotationsInto(std::vector<jpov::Vec3f>* out /*output*/) const;

    // 质点 i 在给定位置序列下的面积加权法线（用关联三角形算）。
    jpov::Vec3f ParticleNormal(uint32_t i,
                               const std::vector<jpov::Vec3f>& pos) const;

    // 三类约束的 PBD 投影（各自读 config_ 的开关与强度）。
    void ProjectShape();
    void ProjectStretch();
    void ProjectBend();
    void ProjectGround();

    // 把质点位置散布到逐顶点数组 vertex_positions_（未焊接写回）。
    void ScatterToVertices();

    ArapSimConfig config_;

    // ── 焊接映射（构建时定，之后不变）──
    size_t particle_count_ = 0;
    std::vector<uint32_t> particle_of_vertex_;   // 全局顶点 → 质点
    std::vector<size_t> mesh_vertex_begin_;      // 各网格在全局顶点空间的前缀和（长度 +1）

    // ── 质点状态（焊接拓扑上的物理量）──
    std::vector<jpov::Vec3f> rest_pos_;   // bind pose 质点位置（原始形状目标）
    std::vector<jpov::Vec3f> pos_;        // 当前质点位置
    std::vector<jpov::Vec3f> prev_pos_;   // 本子步预测前的位置（速度反推用）
    std::vector<jpov::Vec3f> vel_;        // 质点速度

    // ── 约束图 ──
    struct Edge {
        uint32_t a;
        uint32_t b;
        float rest_length;
    };
    std::vector<Edge> edges_;
    // 弯曲约束（"对顶点距离"形式，见 config 里 bend_restore_rate_per_second 的说明）。
    std::vector<Edge> bend_edges_;
    std::vector<std::vector<uint32_t>> ring_;   // 质点的 1-ring 邻域（不含自身）

    // ── 形状匹配：原始目标、当前旋转、薄壳法向增广 ──
    std::vector<jpov::Vec3f> rest_local_centroid_;   // rest_c_i（含自身的邻域质心）
    std::vector<jpov::Vec3f> rot_;                   // 当前的 R_i · (rest_i − rest_c_i)
    std::vector<std::array<uint32_t, 3>> tris_;      // 三角形（质点级）
    std::vector<std::vector<uint32_t>> particle_tris_;  // 质点 → 关联三角形下标
    std::vector<jpov::Vec3f> rest_normal_;           // 质点 rest 面积加权法线
    std::vector<float> ring_scale_sqr_;              // 邻域尺度 δ²（法向增广项权重）

    // 退化旋转计数（诊断用；mutable 因为 shape_residual_rms() 是 const 也会重算一次）。
    mutable size_t degenerate_rotation_count_ = 0;

    // 本子步的形状/边长投影系数（由 rate 与 h 推算，每子步重算）。
    float shape_substep_factor_ = 0.0f;
    float stretch_iter_factor_ = 0.0f;
    float bend_iter_factor_ = 0.0f;

    // 短边阈值（模型 rest 包围盒对角线的 1e-4 倍）。
    //   ⚠️ 真实资产（尤其 Unity/Tripo 导出的多 primitive 模型）里常残留**极短的
    //   退化边**（长度 1e-5 量级）。这类边一拉就爆：近刚性形状投影一步移动 0.01
    //   就能把它拉长 1000 倍 ⇒ 约束力/畸变爆表 ⇒ 整个仿真飞掉（实测路灯首帧畸变
    //   230%、随后飞到 15000）。故：拓扑（1-ring）保留这些边，但**约束求解与畸变
    //   度量都跳过**它们 —— 它们不代表真实几何结构。
    float min_edge_length_ = 0.0f;

    // 本子步哪些质点与地面发生投影（供速度更新时施加切向摩擦）。
    std::vector<uint8_t> ground_contact_;

    // ── 渲染侧位置副本（未焊接，每帧由质点位置散布而来）──
    std::vector<jpov::Vec3f> vertex_positions_;
};

}  // namespace jpov_arap

#endif  // JPOV_DEMO_ARAP_SIM_H_
