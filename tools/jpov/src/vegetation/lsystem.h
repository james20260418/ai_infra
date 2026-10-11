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
};

// ============================ 骨架（拓扑树） ============================

// 一根「棒」：从 start 到 end 的线段 + 半径（用于可视化拓扑）。
struct Segment {
    Vec3f start;
    Vec3f end;
    float radius = 0.0f;
};

// 拓扑树骨架 = 一串可独立绘制的线段（已展平，不含分支拓扑，但保留几何）。
struct Skeleton {
    std::vector<Segment> segments;
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
// 龙爪槐（Sophora japonica 'Pendula'）的签名特征：矮伞形（嫁接）+ 枝条下垂 +
// 末梢扭曲卷曲如龙爪。第三条是纯规则（下垂 + 每步扭转），正是 L-System 主场。
//
// 规则（参数 n/s 为「年龄/进度」计数；角度单位：度）：
//   A(n) : n < 7  -> F(7)[\(n*137)&(55)B(0)][\(n*137+180)&(55)B(0)]A(n+1)
//                   主干：矮、直立，参数化方位角（n×137° 黄金角）绕主干轴 roll 发一对侧枝
//                   （方位用 `\` 绕 H roll，不是 `^`/`+`——否则退化成 2D 扇面）
//   B(s) : s < 5  -> F(3)&(10)B(s+1)                           侧枝「外张相」：先铺开骨架
//   B(s) : s < 18 -> F(3)&(4)\(22)B(s+1)                       侧枝「龙爪相」：下垂 + 每步扭转
//   B(s) : s < 24 -> F(2)[+(35)&(45)L][-(35)&(45)L]B(s+1)      末梢：分小杈挂叶枝
//   B(s) : s >= 24 -> F(1)L                                     收尾
//   L             -> F(1)                                       叶枝（MVP 画成短枝）
//
// 注：角度常量是**待调参**的初值（形似龙爪槐即可），逼真靠后续对着照片迭代。
LSystem BuildDragonClawPagodaTree();

}  // namespace vegetation
}  // namespace jpov

#endif  // JPOV_VEGETATION_LSYSTEM_H_
