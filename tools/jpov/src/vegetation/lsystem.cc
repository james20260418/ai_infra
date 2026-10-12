// JPOV L-System 引擎 + 龟图解释器 —— 实现（GL-free、确定性）。
//
// 结构：
//   §1 splitmix64 RNG
//   §2 算术表达式解析/求值（参数化 L-System 的参数与条件）
//   §3 替换串 / 规则文本解析
//   §4 改写（一代）与展开
//   §5 龟图解释（符号序列 → 线段骨架）
//   §6 内置示例：龙爪槐
//
// 设计见 tools/jpov/docs/jpov_vegetation_design.md。

#include "tools/jpov/src/vegetation/lsystem.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include <glog/logging.h>

namespace jpov {
namespace vegetation {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDeg2Rad = kPi / 180.0f;

// ============================ §2 表达式解析 ============================

void SkipWs(const std::string& s, size_t& i) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
        ++i;
    }
}

Expr ParseExpr(const std::string& s, size_t& i,
               const std::vector<std::string>& vars);

bool IsIdentStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}
bool IsIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// 主表达式：数字 / 参数变量 / ( 子表达式 )。
Expr ParsePrimary(const std::string& s, size_t& i,
                  const std::vector<std::string>& vars) {
    SkipWs(s, i);
    CHECK_LT(i, s.size()) << "表达式意外结束: " << s;
    const char c = s[i];
    if (c == '(') {
        ++i;
        Expr e = ParseExpr(s, i, vars);
        SkipWs(s, i);
        CHECK_LT(i, s.size()) << "表达式缺右括号: " << s;
        CHECK_EQ(s[i], ')') << "表达式缺右括号: " << s;
        ++i;
        return e;
    }
    if (IsIdentStart(c)) {
        const size_t begin = i;
        while (i < s.size() && IsIdentChar(s[i])) {
            ++i;
        }
        const std::string name = s.substr(begin, i - begin);
        int idx = -1;
        for (size_t k = 0; k < vars.size(); ++k) {
            if (vars[k] == name) {
                idx = static_cast<int>(k);
                break;
            }
        }
        CHECK_GE(idx, 0) << "表达式引用未知参数名 '" << name
                         << "'（LHS 未声明）: " << s;
        Expr e;
        e.kind = Expr::Kind::kVar;
        e.var_index = idx;
        return e;
    }
    // 数字（接受前导负号由 ParseUnary 处理，这里只读无符号）。
    CHECK(std::isdigit(static_cast<unsigned char>(c)) || c == '.')
        << "表达式出现非法字符 '" << c << "': " << s;
    const char* begin = s.c_str() + i;
    char* end = nullptr;
    const double v = std::strtod(begin, &end);
    CHECK(end != begin) << "数字解析失败: " << s;
    i += static_cast<size_t>(end - begin);
    Expr e;
    e.kind = Expr::Kind::kNumber;
    e.number = static_cast<float>(v);
    return e;
}

// 一元：前导 +/-。
Expr ParseUnary(const std::string& s, size_t& i,
                const std::vector<std::string>& vars) {
    SkipWs(s, i);
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
        const char op = s[i];
        ++i;
        Expr operand = ParseUnary(s, i, vars);
        if (op == '+') {
            return operand;
        }
        Expr e;
        e.kind = Expr::Kind::kNeg;
        e.a = std::make_unique<Expr>(std::move(operand));
        return e;
    }
    return ParsePrimary(s, i, vars);
}

// 乘除。
Expr ParseMulDiv(const std::string& s, size_t& i,
                 const std::vector<std::string>& vars) {
    Expr left = ParseUnary(s, i, vars);
    while (true) {
        SkipWs(s, i);
        if (i >= s.size() || (s[i] != '*' && s[i] != '/')) {
            break;
        }
        const char op = s[i];
        ++i;
        Expr right = ParseUnary(s, i, vars);
        Expr e;
        e.kind = (op == '*') ? Expr::Kind::kMul : Expr::Kind::kDiv;
        e.a = std::make_unique<Expr>(std::move(left));
        e.b = std::make_unique<Expr>(std::move(right));
        left = std::move(e);
    }
    return left;
}

// 加减（表达式顶层）。
Expr ParseExpr(const std::string& s, size_t& i,
               const std::vector<std::string>& vars) {
    Expr left = ParseMulDiv(s, i, vars);
    while (true) {
        SkipWs(s, i);
        if (i >= s.size() || (s[i] != '+' && s[i] != '-')) {
            break;
        }
        const char op = s[i];
        ++i;
        Expr right = ParseMulDiv(s, i, vars);
        Expr e;
        e.kind = (op == '+') ? Expr::Kind::kAdd : Expr::Kind::kSub;
        e.a = std::make_unique<Expr>(std::move(left));
        e.b = std::make_unique<Expr>(std::move(right));
        left = std::move(e);
    }
    return left;
}

float EvalExpr(const Expr& e, const float* params, int arity) {
    switch (e.kind) {
        case Expr::Kind::kNumber:
            return e.number;
        case Expr::Kind::kVar:
            CHECK_GE(e.var_index, 0);
            CHECK_LT(e.var_index, arity)
                << "表达式引用的参数下标越界（LHS arity=" << arity << "）";
            return params[e.var_index];
        case Expr::Kind::kNeg:
            return -EvalExpr(*e.a, params, arity);
        case Expr::Kind::kAdd:
            return EvalExpr(*e.a, params, arity) + EvalExpr(*e.b, params, arity);
        case Expr::Kind::kSub:
            return EvalExpr(*e.a, params, arity) - EvalExpr(*e.b, params, arity);
        case Expr::Kind::kMul:
            return EvalExpr(*e.a, params, arity) * EvalExpr(*e.b, params, arity);
        case Expr::Kind::kDiv: {
            const float denom = EvalExpr(*e.b, params, arity);
            CHECK(std::abs(denom) > 1.0e-12f) << "表达式除以零";
            return EvalExpr(*e.a, params, arity) / denom;
        }
    }
    LOG(FATAL) << "未知表达式类型";
    return 0.0f;
}

// ============================ §3 替换串 / 规则解析 ============================

// 龟图命令符 + 任意字母均可作为符号（字母包含非终结符与自定义标记）。
bool IsSymbolChar(char c) {
    if (std::isalpha(static_cast<unsigned char>(c))) {
        return true;
    }
    switch (c) {
        case '+': case '-': case '&': case '^': case '\\':
        case '/': case '|': case '[': case ']':
            return true;
        default:
            return false;
    }
}

// 把替换串解析成「符号 + 参数表达式」列表。
std::vector<Production::RhsModule> ParseModules(
    const std::string& text, const std::vector<std::string>& vars) {
    std::vector<Production::RhsModule> out;
    size_t i = 0;
    while (true) {
        SkipWs(text, i);
        if (i >= text.size()) {
            break;
        }
        const char c = text[i];
        CHECK(IsSymbolChar(c))
            << "替换串出现非法符号 '" << c << "': " << text;
        Production::RhsModule m;
        m.symbol = c;
        ++i;
        SkipWs(text, i);
        if (i < text.size() && text[i] == '(') {
            ++i;
            while (true) {
                m.args.push_back(ParseExpr(text, i, vars));
                SkipWs(text, i);
                CHECK_LT(i, text.size()) << "参数列表缺右括号: " << text;
                if (text[i] == ',') {
                    ++i;
                    continue;
                }
                CHECK_EQ(text[i], ')') << "参数列表缺右括号: " << text;
                ++i;
                break;
            }
        }
        out.push_back(std::move(m));
    }
    return out;
}

// 在括号深度 0 处查找条件运算符，拆成 [left, op, right]。
struct CondSplit {
    bool found = false;
    Production::CmpOp op = Production::CmpOp::kLt;
    std::string left;
    std::string right;
};

CondSplit SplitCondition(const std::string& text) {
    CondSplit r;
    int depth = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '(') {
            ++depth;
            continue;
        }
        if (c == ')') {
            --depth;
            continue;
        }
        if (depth != 0) {
            continue;
        }
        // 先看两字符运算符，再看单字符。
        const std::string two = (i + 1 < text.size()) ? text.substr(i, 2) : "";
        if (two == "<=" || two == ">=" || two == "==" || two == "!=") {
            r.found = true;
            r.left = text.substr(0, i);
            r.right = text.substr(i + 2);
            if (two == "<=") r.op = Production::CmpOp::kLe;
            else if (two == ">=") r.op = Production::CmpOp::kGe;
            else if (two == "==") r.op = Production::CmpOp::kEq;
            else r.op = Production::CmpOp::kNe;
            return r;
        }
        if (c == '<' || c == '>') {
            r.found = true;
            r.left = text.substr(0, i);
            r.right = text.substr(i + 1);
            r.op = (c == '<') ? Production::CmpOp::kLt : Production::CmpOp::kGt;
            return r;
        }
    }
    return r;
}

Expr ParseOneExpr(const std::string& text,
                  const std::vector<std::string>& vars) {
    size_t i = 0;
    Expr e = ParseExpr(text, i, vars);
    SkipWs(text, i);
    CHECK_EQ(i, text.size()) << "表达式有未消费的尾巴: '" << text << "'";
    return e;
}

// 解析一条规则文本 → Production。
Production ParseProduction(const std::string& rule_text, float weight) {
    Production p;
    p.text = rule_text;
    p.weight = weight;

    // 拆分 LHS / RHS（接受 -> 与 →）。
    size_t arrow = rule_text.find("->");
    size_t arrow_len = 2;
    if (arrow == std::string::npos) {
        arrow = rule_text.find("\xE2\x86\x92");  // UTF-8 "→"
        arrow_len = 3;
    }
    CHECK(arrow != std::string::npos)
        << "规则缺 '->': " << rule_text;
    const std::string lhs_text = rule_text.substr(0, arrow);
    const std::string rhs_text = rule_text.substr(arrow + arrow_len);

    // LHS 拆成 签名 / 条件。
    const size_t colon = lhs_text.find(':');
    const std::string sig_text =
        (colon == std::string::npos) ? lhs_text : lhs_text.substr(0, colon);
    const std::string cond_text =
        (colon == std::string::npos) ? "" : lhs_text.substr(colon + 1);

    // 解析签名：符号 + 可选 (参数名列表)。
    size_t i = 0;
    SkipWs(sig_text, i);
    CHECK_LT(i, sig_text.size()) << "规则左部为空: " << rule_text;
    CHECK(std::isalpha(static_cast<unsigned char>(sig_text[i])))
        << "规则左部符号必须是字母: " << rule_text;
    p.lhs = sig_text[i];
    ++i;
    SkipWs(sig_text, i);
    std::vector<std::string> vars;
    if (i < sig_text.size() && sig_text[i] == '(') {
        ++i;
        while (true) {
            SkipWs(sig_text, i);
            const size_t begin = i;
            while (i < sig_text.size() &&
                   (IsIdentChar(sig_text[i]))) {
                ++i;
            }
            CHECK_GT(i, begin) << "规则左部参数名为空: " << rule_text;
            vars.push_back(sig_text.substr(begin, i - begin));
            SkipWs(sig_text, i);
            CHECK_LT(i, sig_text.size()) << "规则左部参数列表缺右括号: " << rule_text;
            if (sig_text[i] == ',') {
                ++i;
                continue;
            }
            CHECK_EQ(sig_text[i], ')') << "规则左部参数列表缺右括号: " << rule_text;
            ++i;
            break;
        }
    }
    SkipWs(sig_text, i);
    CHECK_EQ(i, sig_text.size()) << "规则左部有未消费内容: " << rule_text;
    p.arity = static_cast<int>(vars.size());

    // 条件。
    if (!cond_text.empty()) {
        const CondSplit cs = SplitCondition(cond_text);
        CHECK(cs.found) << "规则条件缺运算符: " << rule_text;
        p.cond_lhs = std::make_unique<Expr>(ParseOneExpr(cs.left, vars));
        p.cond_op = cs.op;
        p.cond_rhs = std::make_unique<Expr>(ParseOneExpr(cs.right, vars));
    }

    // 替换串。
    p.rhs = ParseModules(rhs_text, vars);
    return p;
}

// 条件判定（无 cond_lhs 视为恒真）。
bool CondHolds(const Production& p, const float* params, int arity) {
    if (p.cond_lhs == nullptr) {
        return true;
    }
    const float a = EvalExpr(*p.cond_lhs, params, arity);
    const float b = EvalExpr(*p.cond_rhs, params, arity);
    switch (p.cond_op) {
        case Production::CmpOp::kLt: return a <  b;
        case Production::CmpOp::kLe: return a <= b;
        case Production::CmpOp::kGt: return a >  b;
        case Production::CmpOp::kGe: return a >= b;
        case Production::CmpOp::kEq: return a == b;
        case Production::CmpOp::kNe: return a != b;
    }
    return false;
}

// 用匹配到的参数求值一条产生式的右部 → 模块列表。
std::vector<Module> EmitRhs(const Production& p, const float* params,
                            int arity) {
    std::vector<Module> out;
    out.reserve(p.rhs.size());
    for (const Production::RhsModule& rm : p.rhs) {
        Module m;
        m.symbol = rm.symbol;
        m.params.reserve(rm.args.size());
        for (const Expr& e : rm.args) {
            m.params.push_back(EvalExpr(e, params, arity));
        }
        out.push_back(std::move(m));
    }
    return out;
}

// ============================ §4 改写 / 展开 ============================

std::vector<Module> RewriteOnce(const std::vector<Production>& prods,
                                const std::vector<Module>& in, Rng* rng) {
    std::vector<Module> out;
    out.reserve(in.size() * 2);
    for (const Module& m : in) {
        // 收集命中（符号 + 元数 + 条件）的产生式。
        std::vector<const Production*> hits;
        for (const Production& p : prods) {
            if (p.lhs != m.symbol) {
                continue;
            }
            if (p.arity != static_cast<int>(m.params.size())) {
                continue;
            }
            if (CondHolds(p, m.params.data(), static_cast<int>(m.params.size()))) {
                hits.push_back(&p);
            }
        }
        if (hits.empty()) {
            // 无规则命中（如展开到位后的非终结符）→ 原样保留（龟图会忽略未知字母）。
            out.push_back(m);
            continue;
        }
        const Production* chosen = hits[0];
        if (hits.size() > 1) {
            // 加权随机择一。
            float total = 0.0f;
            for (const Production* p : hits) {
                CHECK_GT(p->weight, 0.0f) << "产生式权重必须 > 0";
                total += p->weight;
            }
            float r = rng->NextFloat01() * total;
            for (const Production* p : hits) {
                r -= p->weight;
                if (r <= 0.0f) {
                    chosen = p;
                    break;
                }
            }
        }
        std::vector<Module> emitted =
            EmitRhs(*chosen, m.params.data(), static_cast<int>(m.params.size()));
        for (Module& e : emitted) {
            out.push_back(std::move(e));
        }
    }
    return out;
}

// ============================ §5 龟图 ============================

// 三个向量的小工具（显式分量运算，避免依赖 geom 运算符重载是否齐全）。
inline Vec3f VAdd(const Vec3f& a, const Vec3f& b) {
    return Vec3f(a.x() + b.x(), a.y() + b.y(), a.z() + b.z());
}
inline Vec3f VSub(const Vec3f& a, const Vec3f& b) {
    return Vec3f(a.x() - b.x(), a.y() - b.y(), a.z() - b.z());
}
inline Vec3f VScale(const Vec3f& a, float s) {
    return Vec3f(a.x() * s, a.y() * s, a.z() * s);
}
inline float VDot(const Vec3f& a, const Vec3f& b) {
    return a.x() * b.x() + a.y() * b.y() + a.z() * b.z();
}
inline Vec3f VCross(const Vec3f& a, const Vec3f& b) {
    return Vec3f(a.y() * b.z() - a.z() * b.y(),
                 a.z() * b.x() - a.x() * b.z(),
                 a.x() * b.y() - a.y() * b.x());
}
inline float VLen(const Vec3f& a) { return std::sqrt(VDot(a, a)); }
inline Vec3f VNorm(const Vec3f& a) {
    const float l = VLen(a);
    CHECK_GT(l, 1.0e-12f) << "零向量无法归一化";
    return VScale(a, 1.0f / l);
}

// 绕单位轴 k 旋转 v 角度 θ（Rodrigues 公式）。
Vec3f RotateAround(const Vec3f& v, const Vec3f& axis_unit, float theta) {
    const float c = std::cos(theta);
    const float s = std::sin(theta);
    const Vec3f kxv = VCross(axis_unit, v);
    const float kdv = VDot(axis_unit, v);
    return VAdd(VAdd(VScale(v, c), VScale(kxv, s)),
                VScale(axis_unit, kdv * (1.0f - c)));
}

// 龟图状态：位置 + 正交标架 (H 前进 / L 左 / U 上) + 当前粗细。
struct TurtleState {
    Vec3f pos;
    Vec3f H;   // heading（前进方向）
    Vec3f L;   // left
    Vec3f U;   // up
    float radius = 0.0f;
};

// 正交化标架（H 为权威方向，L = cross(U, H)），消除多次旋转的累积漂移。
void Orthonormalize(TurtleState* st) {
    st->H = VNorm(st->H);
    Vec3f u = VSub(st->U, VScale(st->H, VDot(st->U, st->H)));
    st->U = VNorm(u);
    st->L = VCross(st->U, st->H);
}

void PushSegment(const TurtleState& st, const Vec3f& end, Skeleton* out) {
    Segment seg;
    seg.start = st.pos;
    seg.end = end;
    seg.radius = st.radius;
    out->segments.push_back(seg);
}

// 向性：把朝向 H 朝 T 轻微弯折（含 L/U 同转，保持标架正交）。
// 转角 = strength·|H×T|（H 与 T 平行时不弯）；强度/方向为零则不动。
void ApplyTropism(TurtleState* st, const TurtleConfig& cfg) {
    const float e = cfg.tropism_strength;
    const float tlen = VLen(cfg.tropism);
    if (e <= 0.0f || tlen <= 1.0e-9f) {
        return;
    }
    const Vec3f T = VScale(cfg.tropism, 1.0f / tlen);
    const Vec3f axis = VCross(st->H, T);
    const float s = VLen(axis);
    if (s <= 1.0e-9f) {
        return;   // H 已与 T 共线 → 无可弯
    }
    const Vec3f k = VScale(axis, 1.0f / s);
    const float ang = e * s;
    st->H = RotateAround(st->H, k, ang);
    st->L = RotateAround(st->L, k, ang);
    st->U = RotateAround(st->U, k, ang);
}

// 取一个「转向角」参数（度→弧度）。缺参数视为 0（不转），避免误 crash。
float AngleParam(const Module& m) {
    if (m.params.empty()) {
        return 0.0f;
    }
    return m.params[0] * kDeg2Rad;
}

// 龟图符号 → 几何：`F f + - & ^ \ / | [ ]` 如上；另：
//   `l` = 记录一个叶位（LeafSite，挂在当前位姿），不画几何（叶朝向：axis=H / normal=L）。
//   其余字母（非终结符残留）忽略。
Skeleton Interpret(const std::vector<Module>& modules, const TurtleConfig& cfg) {
    CHECK_GT(cfg.length_scale, 0.0f);
    CHECK_GT(cfg.base_radius, 0.0f);
    CHECK_GT(cfg.min_radius, 0.0f);
    CHECK_GT(cfg.radius_taper_per_segment, 0.0f);
    CHECK_LE(cfg.radius_taper_per_segment, 1.0f);
    CHECK_GT(cfg.radius_taper_per_branch, 0.0f);
    CHECK_LE(cfg.radius_taper_per_branch, 1.0f);

    TurtleState st;
    st.pos = Vec3f(0.0f, 0.0f, 0.0f);
    st.H = Vec3f(0.0f, 1.0f, 0.0f);   // 主干起始向上
    st.U = Vec3f(0.0f, 0.0f, 1.0f);
    st.L = Vec3f(-1.0f, 0.0f, 0.0f);  // L = cross(U, H)
    st.radius = cfg.base_radius;

    Skeleton skel;
    std::vector<TurtleState> stack;
    // 当前正在累积的枝链下标（-1 = 无）；分叉 `[`/`]` 与空移 `f` 会切断枝链。
    int cur_polyline = -1;
    // 分叉领口：`[` 时记下的母枝半径；供「下一条新建的枝链」作为子枝缝合参考。
    // -1 = 无（根链或母链续段，不缝合）。
    float pending_attach_radius = -1.0f;
    const auto finish_polyline = [&cur_polyline]() { cur_polyline = -1; };

    for (const Module& m : modules) {
        switch (m.symbol) {
            case 'F':
            case 'G': {
                const float len_units = m.params.empty() ? 1.0f : m.params[0];
                const float len = len_units * cfg.length_scale;
                Orthonormalize(&st);
                const Vec3f end = VAdd(st.pos, VScale(st.H, len));
                PushSegment(st, end, &skel);
                // 枝链：续接当前链，或起一条新链（起点 = 当前位姿 + 当前半径）。
                if (cur_polyline < 0) {
                    skel.polylines.push_back(Polyline{});
                    cur_polyline = static_cast<int>(skel.polylines.size()) - 1;
                    // 若是刚 `[` 出来的子枝，记下母枝半径供分叉领口缝合。
                    if (pending_attach_radius > 0.0f) {
                        skel.polylines[cur_polyline].attach_radius = pending_attach_radius;
                        pending_attach_radius = -1.0f;
                    }
                    skel.polylines[cur_polyline].points.push_back(st.pos);
                    skel.polylines[cur_polyline].radii.push_back(st.radius);
                }
                const float new_radius = std::max(
                    cfg.min_radius, st.radius * cfg.radius_taper_per_segment);
                skel.polylines[cur_polyline].points.push_back(end);
                skel.polylines[cur_polyline].radii.push_back(new_radius);
                st.pos = end;
                st.radius = new_radius;
                ApplyTropism(&st, cfg);   // 向性：每段后微调朝向
                break;
            }
            case 'f': {
                const float len_units = m.params.empty() ? 1.0f : m.params[0];
                Orthonormalize(&st);
                finish_polyline();   // 空移切断枝链（不画）
                pending_attach_radius = -1.0f;
                st.pos = VAdd(st.pos, VScale(st.H, len_units * cfg.length_scale));
                break;
            }
            case '+':
                Orthonormalize(&st);
                st.H = RotateAround(st.H, st.U, AngleParam(m));
                st.L = RotateAround(st.L, st.U, AngleParam(m));
                break;
            case '-':
                Orthonormalize(&st);
                st.H = RotateAround(st.H, st.U, -AngleParam(m));
                st.L = RotateAround(st.L, st.U, -AngleParam(m));
                break;
            case '&':
                Orthonormalize(&st);
                st.H = RotateAround(st.H, st.L, AngleParam(m));
                st.U = RotateAround(st.U, st.L, AngleParam(m));
                break;
            case '^':
                Orthonormalize(&st);
                st.H = RotateAround(st.H, st.L, -AngleParam(m));
                st.U = RotateAround(st.U, st.L, -AngleParam(m));
                break;
            case '\\':
                Orthonormalize(&st);
                st.L = RotateAround(st.L, st.H, AngleParam(m));
                st.U = RotateAround(st.U, st.H, AngleParam(m));
                break;
            case '/':
                Orthonormalize(&st);
                st.L = RotateAround(st.L, st.H, -AngleParam(m));
                st.U = RotateAround(st.U, st.H, -AngleParam(m));
                break;
            case '|':
                Orthonormalize(&st);
                st.H = RotateAround(st.H, st.U, kPi);
                st.L = RotateAround(st.L, st.U, kPi);
                break;
            case '[':
                finish_polyline();   // 分支点切断母枝链
                stack.push_back(st);
                pending_attach_radius = st.radius;   // 母枝半径（供子枝领口缝合）
                st.radius = std::max(cfg.min_radius,
                                     st.radius * cfg.radius_taper_per_branch);
                break;
            case ']':
                CHECK(!stack.empty()) << "龟图遇到 ']' 但栈为空（括号不配对）";
                finish_polyline();   // 出分支后母枝继续，需起新链
                st = stack.back();
                stack.pop_back();
                pending_attach_radius = -1.0f;
                break;
            case 'l': {
                // 叶位标记：记录着生点与朝向（轴向 = 前进方向 H，法线 = L 与 H 垂直）。
                Orthonormalize(&st);
                LeafSite site;
                site.position = st.pos;
                site.axis = st.H;
                site.normal = st.L;
                skel.leaf_sites.push_back(site);
                break;
            }
            default:
                // 非终结符残留（无规则可改写）或未识别字母 → 龟图不动作。
                break;
        }
    }
    return skel;
}

}  // namespace

// ============================ §1 RNG ============================

uint64_t Rng::NextU64() {
    // splitmix64。
    state_ += 0x9E3779B97F4A7C15ULL;
    uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

float Rng::NextFloat01() {
    // 取高 53 位映射到 [0,1)（与 double 尾数同宽）。
    return static_cast<float>((NextU64() >> 11) * 0x1.0p-53);
}

// ============================ LSystemBuilder ============================

void LSystemBuilder::SetAxiom(const std::string& axiom_text) {
    axiom_text_ = axiom_text;
}

void LSystemBuilder::AddRule(const std::string& rule_text, float weight) {
    CHECK_GT(weight, 0.0f) << "规则权重必须 > 0: " << rule_text;
    rules_.push_back(PendingRule{rule_text, weight});
}

LSystem LSystemBuilder::Build() const {
    LSystem sys;
    const std::vector<Production::RhsModule> axiom_modules =
        ParseModules(axiom_text_, /*vars=*/{});
    for (const Production::RhsModule& rm : axiom_modules) {
        Module m;
        m.symbol = rm.symbol;
        for (const Expr& e : rm.args) {
            m.params.push_back(EvalExpr(e, /*params=*/nullptr, /*arity=*/0));
        }
        sys.axiom_.push_back(std::move(m));
    }
    for (const PendingRule& pr : rules_) {
        sys.productions_.push_back(ParseProduction(pr.text, pr.weight));
    }
    return sys;
}

// ============================ LSystem::Expand ============================

std::vector<Module> LSystem::Expand(int generations, uint64_t seed) const {
    CHECK_GE(generations, 0) << "generations 必须 >= 0";
    Rng rng(seed);
    std::vector<Module> current = axiom_;
    for (int g = 0; g < generations; ++g) {
        current = RewriteOnce(productions_, current, &rng);
    }
    return current;
}

// ============================ GenerateSkeleton ============================

Skeleton GenerateSkeleton(const LSystem& sys, const TurtleConfig& cfg,
                          int generations, uint64_t seed,
                          std::vector<Module>* final_modules_out) {
    std::vector<Module> modules = sys.Expand(generations, seed);
    if (final_modules_out != nullptr) {
        *final_modules_out = modules;
    }
    return Interpret(modules, cfg);
}

// ============================ §6 内置示例：龙爪槐 ============================

LSystem BuildDragonClawPagodaTree() {
    LSystemBuilder b;
    b.SetAxiom("A(0)");
    // ── 主干 A(n)：矮而直立的砧木（嫁接干），到嫁接高度即停；**干上不长枝** ──
    //    （旧版在干上每层发枝 → 长成松塔式轮生；龙爪槐应是「清干 + 顶部嫁接冠」。）
    b.AddRule("A(n) : n < 5 -> F(9)A(n + 1)");
    // ── 嫁接点：从干顶放射 6 条骨架枝，绕干约 60° 均布（俯仰略作抖动破对称），
    //    初始离垂直 26°~38°（≈ 水平以上 52°~64°），先把伞形穹顶拱起来。
    //    ⚠️ 绕主干轴分发方位必须用 `\`（绕 H roll），先 `\` 定方位再 `&` 抬/俯；
    //    不能用 `+`（绕 U yaw）——因主干 U=+Z 不是主干轴，`+` 是把主干本身扳歪；
    //    也不能用 `^`（绕 L pitch）——那会让所有枝落在同一平面，长成 2D 扇面。
    b.AddRule("A(n) : n >= 5 -> [\\(0)&(30)B(0)][\\(60)&(36)B(0)][\\(120)&(26)B(0)]"
              "[\\(180)&(38)B(0)][\\(240)&(28)B(0)][\\(300)&(34)B(0)]");
    // ⚠️ 各相位的条件必须**互斥**（如 `s<K` 与 `s>=K`）：若写成嵌套区间（s<3 与 s<6 同时
    //    命中），引擎会按权重**随机**择一，相位就变成掷骰子、树形不再确定。
    // ── 主枝：① 拱起段（伞盖弧）─────────────────────────────────
    b.AddRule("B(s) : s < 3 -> F(5)&(14)B(s + 1)");
    b.AddRule("B(s) : s >= 3 -> C(0)");
    //   ② 过顶段（越过穹顶、趋近垂直向下）
    b.AddRule("C(s) : s < 3 -> F(5)&(24)C(s + 1)");
    b.AddRule("C(s) : s >= 3 -> D(0)");
    //   ③ 下垂段：继续下俯到近垂直，挂叶位 `l`，并从内侧抽出下垂短枝 P（垂帘）
    b.AddRule("D(s) : s < 6 -> F(5)&(14)\\(6)l[&(30)\\(10)P(0)]D(s + 1)");
    b.AddRule("D(s) : s >= 6 -> E(0)");
    //   ④ 爪尖：末端渐次扭转卷曲（龙爪签名）
    b.AddRule("E(s) : s < 3 -> F(3)&(10)\\(10)lE(s + 1)");
    b.AddRule("E(s) : s >= 3 -> F(2)&(4)\\(28)l");
    // ── 下垂短枝 P(s)/Q(s)：垂帘——直接下垂 + 渐增扭转卷曲 + 密集叶位 ──
    b.AddRule("P(s) : s < 5 -> F(4)&(12)\\(22)lP(s + 1)");
    b.AddRule("P(s) : s >= 5 -> Q(0)");
    b.AddRule("Q(s) : s < 4 -> F(3)&(14)\\(26)lQ(s + 1)");
    b.AddRule("Q(s) : s >= 4 -> F(2)&(16)\\(20)l");
    return b.Build();
}

// ============================ 预实验：参比树型 ============================

LSystem BuildRecursiveTree() {
    // Honda/ABOP 式：母枝→两子枝，长度按收缩比缩短（0.74 / 0.60）；
    // 每节先绕干滚 137°（黄金角）再分叉 ⇒ 分枝平面逐层旋转、填满 3D（否则像 Honda 原型那样退化成平面）。
    // 走到阈值（0.06）后只挂一个叶位。自相似 ⇒ 越往末梢越细越密的分形层次。
    // 向性由 TurtleConfig 控制（非本规则），见 BuildRecursiveTree 上方注释。
    LSystemBuilder b;
    b.SetAxiom("A(1)");
    b.AddRule("A(s) : s > 0.06 -> \\(137)F(s)[\\(0)&(30)A(s * 0.74)]"
              "[\\(137)&(30)A(s * 0.60)]");
    b.AddRule("A(s) : s <= 0.06 -> F(s)l");
    return b.Build();
}

LSystem BuildRecursiveTreeStochastic() {
    // 同上，但每个节点按权重随机二选一（两种分叉姿态）→ 个体差异（现有引擎的加权随机）。
    LSystemBuilder b;
    b.SetAxiom("A(1)");
    b.AddRule("A(s) : s > 0.07 -> \\(137)F(s)[\\(0)&(30)A(s * 0.74)]"
              "[\\(137)&(30)A(s * 0.60)]", /*weight=*/1.0f);
    b.AddRule("A(s) : s > 0.07 -> \\(95)F(s)[\\(25)&(42)A(s * 0.68)]"
              "[\\(165)&(22)A(s * 0.66)]", /*weight=*/1.0f);
    b.AddRule("A(s) : s <= 0.07 -> F(s)l");
    return b.Build();
}

LSystem BuildJunctionTestTree() {
    // 无规则；axiom 直接交给龟图。在干中点 `[` 抽一条 40° 侧枝再 `]` 回干继续。
    LSystemBuilder b;
    b.SetAxiom("F(10)[&(40)F(8)]F(10)");
    return b.Build();
}

// 杉树：高窄锥形。主干 A 逐段上升；每个高度一圈 6 条侧枝 E，枝长随高度递减（14-n）
// ⇒ 锥形；侧枝每段微俯 4°（微垂）。
LSystem BuildFirTree() {
    LSystemBuilder b;
    b.SetAxiom("A(0)");
    b.AddRule("A(n) : n < 14 -> F(6)[\\(0)&(82)E(14 - n)][\\(60)&(82)E(14 - n)]"
              "[\\(120)&(82)E(14 - n)][\\(180)&(82)E(14 - n)]"
              "[\\(240)&(82)E(14 - n)][\\(300)&(82)E(14 - n)]A(n + 1)");
    b.AddRule("A(n) : n >= 14 -> F(4)F(4)F(4)l");
    b.AddRule("E(m) : m > 1 -> F(3)&(4)lE(m - 1)");
    b.AddRule("E(m) : m <= 1 -> F(3)l");
    return b.Build();
}

// 松树：下部裸干（8 段无枝）→ 上部 6 层轮生大枝 C（每层 6 条 G）→ 冠圆/伞；枝上挂针簇 l。
LSystem BuildPineTree() {
    LSystemBuilder b;
    b.SetAxiom("A(0)");
    b.AddRule("A(n) : n < 8 -> F(7)A(n + 1)");
    b.AddRule("A(n) : n >= 8 -> C(0)");
    b.AddRule("C(n) : n < 6 -> F(5)[\\(0)&(74)G(6)][\\(60)&(74)G(6)][\\(120)&(74)G(6)]"
              "[\\(180)&(74)G(6)][\\(240)&(74)G(6)][\\(300)&(74)G(6)]C(n + 1)");
    b.AddRule("C(n) : n >= 6 -> F(5)l");
    b.AddRule("G(m) : m > 0 -> F(4)&(2)\\(25)lG(m - 1)");
    b.AddRule("G(m) : m <= 0 -> F(2)l");
    return b.Build();
}

// 灌木：几乎无主干（1 段）→ 基部放射 6 条枝 S → 每条反复二叉、短节宽角、沿枝挂叶 → 密丛。
LSystem BuildShrub() {
    LSystemBuilder b;
    b.SetAxiom("A(0)");
    b.AddRule("A(n) : n < 1 -> F(3)A(n + 1)");
    b.AddRule("A(n) : n >= 1 -> [\\(0)&(42)S(0)][\\(64)&(52)S(0)][\\(128)&(34)S(0)]"
              "[\\(196)&(50)S(0)][\\(256)&(40)S(0)][\\(316)&(46)S(0)]");
    b.AddRule("S(s) : s < 7 -> F(4)[\\(0)&(30)S(s + 1)][\\(128)&(38)S(s + 1)]l");
    b.AddRule("S(s) : s >= 7 -> F(3)l");
    return b.Build();
}

}  // namespace vegetation
}  // namespace jpov
