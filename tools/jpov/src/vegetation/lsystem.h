// JPOV L-System（林登梅耶系统）—— 植被程序化生成的第一步：拓扑骨架。
//
// 本文件提供一个**参数化、带条件、可随机**的 L-System 引擎 + 龟图（turtle）
// 解释器，输出一棵树的**拓扑骨架**（一串带粗细的线段，见 Skeleton）。
// 它刻意做成 **GL-free、无渲染依赖、确定性**：同 seed 同参数必得同结果，
// 便于单测与「gold 逐像素可复现」的沙盒约定。
//
// ── 什么是 L-System（一句话）──
// 植物是「一句话」：从公理（axiom）出发，按一组**产生式规则**反复改写字符串，
// 最后用一个「龟图」把字符串解释成几何（前进/转向/分叉）。规则驱动 ⇒ 形状
// 可控、可复现，是「有形态签名」的树种（如龙爪槐的下垂卷爪）的天然工具。
//
// ── 引擎能力（本 MVP 覆盖）──
//   · 参数化：符号带 0..k 个数值参数，如 F(6) / &(24) / B(s)。
//   · 条件产生式：A(n) : n < 7 -> ...（多条同 LHS 规则按条件择一）。
//   · 表达式：参数名 + 数字 + − 四则 + 括号（如 B(s + 1)、F(s * 0.6)）。
//   · 随机（可选）：同 LHS 同条件命中多条规则时按 weight 加权择一。
//   · 龟图：F 前进画线、f 前进不画、+ − & ^ \ / | 转向、[ ] 入栈/出栈。
//
// ── 与「拓扑树」的关系（存储视角）──
// 本引擎的**真值**是「规则 + 参数 + seed」这套程序；Skeleton/网格都是**派生物**。
// 故将来若要做「配方文件」，存的是程序而不是展开结果（见 docs/jpov_vegetation_design.md）。
//
// 设计见 tools/jpov/docs/jpov_vegetation_design.md。

#ifndef JPOV_VEGETATION_LSYSTEM_H_
#define JPOV_VEGETATION_LSYSTEM_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <glog/logging.h>
#include "geom/common/vec.h"

namespace jpov {
namespace vegetation {

// 复用 geom 库的向量类型（与渲染层一致，方便后续直接转 MeshData）。
using Vec3f = geom::Vec3<float>;

// ============================ 确定性随机数 ============================
//
// 基于 splitmix64 的可复现 RNG：同 seed 必得同序列（跨平台、跨进程一致），
// 满足 JPOV「确定性沙盒」要求。仅用于随机 L-System 的规则择一。
class Rng {
public:
    explicit Rng(uint64_t seed) : state_(seed) {}

    // 下一个 64 位随机数（splitmix64 变换）。
    uint64_t NextU64();

    // [0, 1) 浮点随机数（取高 53 位，与 double 尾数同宽）。
    float NextFloat01();

private:
    uint64_t state_;
};

// ============================ 参数化符号（Module） ============================

// L-System 字符串里的一个「符号 + 参数」。例如 F(6) → {symbol='F', params={6}}，
// B(s + 1) 在参数 s=3 时求值为 {symbol='B', params={4}}。
struct Module {
    char symbol = 0;
    std::vector<float> params;
};

// ============================ 表达式 AST ============================
//
// 产生式右侧参数 / 条件里出现的算术表达式。支持：数字、参数变量、一元负号、
// 二元 + − * /、括号。变量用「参数名」引用（在 LHS 里声明的名字，见 Production）。
struct Expr {
    enum class Kind { kNumber, kVar, kAdd, kSub, kMul, kDiv, kNeg };

    Kind kind = Kind::kNumber;
    float number = 0.0f;   // kNumber
    int   var_index = 0;   // kVar：LHS 参数列表里的下标（0 起）
    std::unique_ptr<Expr> a;   // 一元/二元操作数
    std::unique_ptr<Expr> b;   // 二元第二操作数（一元为空）
};

// ============================ 产生式规则 ============================
//
// 形如：  A(n) : n < 7  ->  F(7)[&(62)B(0)]A(n + 1)
//         └符号┘ └─条件─┘   └────── 替换串 ──────┘
// 无参数符号（如 L -> F(1)）：arity = 0，cond 为空（恒成立）。
struct Production {
    char lhs = 0;      // 左部符号
    int  arity = 0;    // 左部参数个数

    // 条件（可选）：cond_lhs <cond_op> cond_rhs。cond_lhs 为空 = 无条件。
    std::unique_ptr<Expr> cond_lhs;
    enum class CmpOp { kLt, kLe, kGt, kGe, kEq, kNe };
    CmpOp cond_op = CmpOp::kLt;
    std::unique_ptr<Expr> cond_rhs;

    // 替换串：一串「符号 + 参数表达式」。
    struct RhsModule {
        char symbol = 0;
        std::vector<Expr> args;
    };
    std::vector<RhsModule> rhs;

    float weight = 1.0f;   // 随机择一权重（>0）
    std::string text;      // 原始规则文本（报错/调试用）
};

// ============================ L-System 定义 ============================

class LSystemBuilder;

// 一套 L-System：公理 + 产生式集。构建后只读。
class LSystem {
public:
    // 公理解析后的符号序列。
    const std::vector<Module>& axiom() const { return axiom_; }
    // 产生式集（按加入顺序，条件择一时先匹配先生效）。
    const std::vector<Production>& productions() const { return productions_; }

    // 展开 generations 代（每代全串并行改写一次），返回最终符号序列。
    // seed 仅影响随机规则择一；确定性语法下与 seed 无关。
    std::vector<Module> Expand(int generations, uint64_t seed) const;

private:
    friend class LSystemBuilder;
    std::vector<Module> axiom_;
    std::vector<Production> productions_;
};

// L-System 构造器：支持以简短文本逐条加规则（可读、可测）。
//
// 规则文本语法（容错空白；`->` 亦可写 `→`）：
//     <符号>[(<参数名>[,<参数名>]*)] [: <条件>] -> <替换串>
// 例：
//     "A(n) : n < 7 -> F(7)[&(62)B(0)]A(n + 1)"
//     "L -> F(1)"
// 条件运算符：<  <=  >  >=  ==  !=
// 替换串里的符号：命令符 F f + - & ^ \ / | [ ] 与任意字母（非终结符/自定义）。
//
// Pre-condition: 规则文本语法合法，否则 AddRule/Build 时 LOG(FATAL) crash。
class LSystemBuilder {
public:
    void SetAxiom(const std::string& axiom_text);
    // weight：随机规则权重（>0）。同 LHS 同条件命中多条时按权重择一。
    void AddRule(const std::string& rule_text, float weight = 1.0f);
    LSystem Build() const;

private:
    std::string axiom_text_;
    struct PendingRule {
        std::string text;
        float weight = 1.0f;
    };
    std::vector<PendingRule> rules_;
};

// ============================ 龟图配置 ============================

// 龟图把符号序列解释成几何时的全局参数（与具体树种无关的「画法」）。
struct TurtleConfig {
    // 长度单位换算：F(x) 的世界长度 = x * length_scale（米）。
    // 让规则里可以写「整数步」（如 F(7)）而整体缩放不变形。
    // Pre-condition: > 0
    float length_scale = 0.1f;

    // 起始粗细（米）：主干最底部线段的半径。
    // Pre-condition: > 0
    float base_radius = 0.06f;

    // 每画一段 F 后的粗细衰减因子（沿枝递减），∈ (0,1]。
    float radius_taper_per_segment = 0.94f;

    // 每进入一次 [ 分叉后的粗细衰减因子（子枝比母枝细），∈ (0,1]。
    float radius_taper_per_branch = 0.60f;

    // 粗细下限（米）：避免远端趋零。Pre-condition: > 0
    float min_radius = 0.002f;

    // ── 向性 tropism（2026-10-12 新增）──
    // 每画完一段 F/G 后，把前进方向 H 朝 T 微偏一点，模拟重力/向光对枝条的持续弯折。
    // ABOP：绕轴 k=(H×T) 旋转，转角 ∝ strength·|H×T|（H 与 T 平行时不弯）。
    //   · 重力（垂枝型如龙爪槐）：T = (0, -1, 0)，枝条不断向下掉；
    //   · 向光（挺拔型）：T = (0, +1, 0)。
    // tropism_strength == 0（默认）或 tropism 为零向量 → **关闭**（零回归）。
    geom::Vec3<float> tropism = {0.0f, 0.0f, 0.0f};   // T 方向（需非零向量才生效）
    float tropism_strength = 0.0f;                    // e（敏感度），建议 0.10 ~ 0.40
};

// ============================ 骨架（拓扑树） ============================

// 一根「棒」：从 start 到 end 的线段 + 半径（用于可视化拓扑）。
struct Segment {
    Vec3f start;
    Vec3f end;
    float radius = 0.0f;
};

// 一条「枝链」：连续前进画出的一串点 + 逐点半径（供管状 mesher 扫掠用）。
// 分叉（`[`/`]`）与空移（`f`）会切断枝链——故每段枝链是「从上一个分叉点到下一个分叉点」
// 的连通折线。
struct Polyline {
    std::vector<Vec3f> points;   // 至少 2 个点
    std::vector<float> radii;    // 与 points 等长（逐点管半径）
};

// 一条「叶位」：叶卡片要摆在哪 + 该处朝向。龟图遇到叶标记符号 `l` 时记录一个。
struct LeafSite {
    Vec3f position;   // 叶柄着生点（世界）
    Vec3f axis;       // 叶主轴方向（卡片局部 +Y）
    Vec3f normal;     // 叶面法线（卡片局部 +Z），与 axis 垂直
};

// 拓扑树骨架：
//   · segments  —— 展平后的每一根棒（可视化/统计，逐 F 一段）
//   · polylines —— 连通枝链（管状 mesher 的输入，保证连续扫掠平滑）
//   · leaf_sites—— 叶片卡片的着生点（叶标记 `l` 处；管网格忽略之）
struct Skeleton {
    std::vector<Segment> segments;
    std::vector<Polyline> polylines;
    std::vector<LeafSite> leaf_sites;
};

// 由 L-System 生成骨架：Expand → 龟图解释。
//
// 参数：
//   sys         — 已构建的 L-System
//   cfg         — 龟图画法参数
//   generations — 展开代数（越多越完整；不足则只长出部分）
//   seed        — 随机种子（确定性语法下无影响）
//   final_modules_out — 可选输出：展开后的最终符号序列（调试/测试用）
//
// 返回：展平的线段骨架。
Skeleton GenerateSkeleton(const LSystem& sys, const TurtleConfig& cfg,
                          int generations, uint64_t seed,
                          std::vector<Module>* final_modules_out = nullptr);

// ============================ 内置示例：龙爪槐 ============================
//
// 龙爪槐（Sophora japonica 'Pendula'）的签名特征：**嫁接矮干** + **伞形穹顶**
// （骨架枝外拱越顶）+ **细枝长垂如帘** + 末梢扭曲卷曲如龙爪。后三条正是 L-System 主场。
//
// 规则（参数 n/s 为「年龄/进度」计数；角度单位：度）：
//   A(n) : n < 5  -> F(9)A(n+1)                   主干：矮、直立、**干上不发枝**（到嫁接高度即停）
//   A(n) : n >= 5 -> [\(0)&(30)B(0)][\(60)&(36)B(0)]…
//                   嫁接点：绕干 ~60° 均布 6 条骨架枝（方位用 `\` 绕 H roll，
//                   不是 `^`/`+`——否则退化成 2D 扇面或把主干扳歪）
//   B(s) : s < 3  -> F(5)&(14)B(s+1)              主枝①「拱起段」：只加大俯角 → 伞盖弧
//   B(s) : s >= 3 -> C(0)                          转相位
//   C(s) : s < 3  -> F(5)&(24)C(s+1)              主枝②「过顶段」：弧越顶、趋近垂直向下
//   C(s) : s >= 3 -> D(0)
//   D(s) : s < 6  -> F(5)&(14)\(6)l[…]D(s+1)        主枝③「下垂段」：继续下俯 + 挂叶 + 抽垂帘短枝 P
//   D(s) : s >= 6 -> E(0)
//   E(s) : s < 3  -> F(3)&(10)\(10)lE(s+1)          主枝④「爪尖」：末端渐次扭转卷曲（龙爪）
//   E(s) : s >= 3 -> F(2)&(4)\(28)l
//   P(s) : s < 5  -> F(4)&(12)\(22)lP(s+1)          下垂短枝：直接下垂 + 扭转 + 挂叶
//   P(s) : s >= 5 -> Q(0)
//   Q(s) : s < 4  -> F(3)&(14)\(26)lQ(s+1)
//   Q(s) : s >= 4 -> F(2)&(16)\(20)l               短枝收尾
//
// ⚠️ 各相位的条件必须**互斥**（s<K 与 s>=K），不能写成嵌套区间（s<3 与 s<6 同时命中）——
//    否则引擎按权重**随机**择一，相位变骰子、树形不再确定（旧版即踩此坑）。
// 注：角度常量是**待调参**的初值（形似龙爪槐即可），逼真靠后续对着照片迭代。
LSystem BuildDragonClawPagodaTree();

// ============================ 预实验：参比树型（现有引擎即可表达）============================
//
// 目的（2026-10-12 Danis「预实验」）：只用现有 L-System 引擎（规则文本，不新增算法），
// 把「拓扑能长成啥样」跑出来看看，作为「还需哪些算法」的对照。三个参比：
//   · 递归自相似（Honda/ABOP 式）：母枝→两子枝，长度按收缩比缩短、黄金角 137.5° 绕干。
//   · 递归 + 「穷人向性」：每段前插一个固定俯角 `&(k)`，近似 gravity/phototropism。
//   · 递归 + 随机：同符号多条加权规则 → 个体差异（现有引擎已支持）。
// 真正的向性（α = e·(H×T)）与管道模型粗细等**尚未实现**，见预实验报告。
// 注：向性不进规则，是龟图级参数（`TurtleConfig.tropism`），由 viewer 滑条/CLI 控制。

// 递归自相似树（Honda/ABOP 式）。每节绕干滚 137°（黄金角）使分枝填满 3D。
LSystem BuildRecursiveTree();
// 递归自相似树 + 随机（同符号加权择一）。
LSystem BuildRecursiveTreeStochastic();

}  // namespace vegetation
}  // namespace jpov

#endif  // JPOV_VEGETATION_LSYSTEM_H_
