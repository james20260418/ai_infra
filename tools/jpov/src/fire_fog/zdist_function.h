// JPOV Fire-Fog — ZDist（逐像素深度函数）CPU 参考实现
//
// 设计见 tools/jpov/docs/jpov_fire_fog_design.md §14（ZDist）与 §16（本 CPU 参考）。
//
// 目的：把 shader 里「叠加 z 段 → 降维到 ≤8 控制点 → 末端积分」这套操作，先在 CPU 上
// 用可直接单测的代码实现一遍，作为 shader 的**对照 / 真值**（debug、gold、回归）。
//
// 约定（camera-forward）：
//   - 自变量 z = 线性视深（米），自相机向场景递增；函数域 = [z_near, z_far]。
//   - τd(z) = 从 z_near 累积到 z 的**光学深度** ∫σ dz（标量，单调不减；τd(z_near)=0）。
//   - Ed(z) = 从 z_near 累积到 z 的**源累积** ∫E dz（RGB 各分量单调不减；Ed(z_near)=0）。
//     ⚠️ 存 τ（可加），不存透射率 T（乘性）——否则退化成薄介质一阶近似（浓雾过亮）。
//   - 末端积分：τ_total = τd(z_far)；T = exp(−τ_total)；
//       S = ∫_{z_near}^{z_far} exp(−τd(z)) dEd(z)；L_out = L_scene·T + S。
//   - 与设计文档「z 自墙深累积到最近」是同一积分的反向记法，数学等价（这里取相机正向，
//     读起来更直接）。
//
// 两阶段（设计文档 §14.3）：
//   1. 精确累加（ZDistAccumulator）：段以**加法**叠加（乱序可加、免排序），控制点数不受限；
//   2. 降采样（ZDistFunction::Reduce）：压到 ≤8 控制点，**端点保留 ⇒ 总上升量守恒**
//      （τ_total / Ed_total 逐位不变）。
//
// GL-free、可单测。控制点用 geom::math::SizeLimitedPiecewiseLinearFunction 承载（即
// 「SLPWL 作为积分和」）。

#ifndef JPOV_SRC_FIRE_FOG_ZDIST_FUNCTION_H_
#define JPOV_SRC_FIRE_FOG_ZDIST_FUNCTION_H_

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include <glog/logging.h>

#include "geom/math/size_limited_piecewise_linear_function.h"
#include "tools/jpov/interface/render_command.h"  // Vec3f

namespace jpov {

// ZDist 控制点上限（设计锁定 8；见 jpov_fire_fog_design.md §14.9）。
inline constexpr int kZDistControlPoints = 8;

// 一条视线穿过一团雾的一个 z 段：在 [z0, z1] 上近似恒定。
//
// 该段对 ZDist 的贡献 = 在 [z0,z1] 上叠一条 ramp：
//   τd 上升 σ·L（L = z1 − z0），Ed 上升 σ·L_in·L。ramp 在 z0 之前为 0、z1 之后为平台。
struct ZDistSegment {
    double z0 = 0.0;                     // 近端视深（米）
    double z1 = 0.0;                     // 远端视深（米），>= z0
    double sigma = 0.0;                  // 消光系数（1/m），>= 0
    Vec3f emission = Vec3f(0.0f, 0.0f, 0.0f);  // 段内散射亮度 L_in（HDR，各分量 >= 0）
};

namespace zdist_detail {

// 源积分的每条子区间权重：∫_0^1 exp(−x·t) dt = (1 − e^{−x}) / x；x→0 时 → 1。
// （对分段线性函数做 Stieltjes 积分的闭式，见 Integrate 注释。）
inline double ExpIntegralRatio(double x) {
    if (std::abs(x) < 1e-6) {
        // 泰勒展开，避免 0/0。
        return 1.0 - 0.5 * x + (1.0 / 6.0) * x * x;
    }
    return (1.0 - std::exp(-x)) / x;
}

// ramp 归一化因子：x <= x0 → 0；x >= x1 → 1；中间线性。[x0, x1] 必须有效（x1 > x0）。
inline double RampFactor(double x, double x0, double x1) {
    if (x <= x0) {
        return 0.0;
    }
    if (x >= x1) {
        return 1.0;
    }
    return (x - x0) / (x1 - x0);
}

// 对一条单调分段线性 ZDist（taus/eds 与 zs 一一对应、zs 升序）做末端源积分。
// 输出 S_total（RGB）；同时给出 T_total 与 L_out。zs 至少 2 个点。
// 预条件：taus.size() == eds.size() == zs.size() >= 2，zs 严格递增。
inline void IntegratePwl(const std::vector<double>& zs,
                         const std::vector<double>& taus,
                         const std::vector<Vec3f>& eds,
                         const Vec3f& L_scene,
                         float* T_total /*output*/,
                         Vec3f* S_total /*output*/,
                         Vec3f* L_out /*output*/) {
    CHECK(T_total != nullptr && S_total != nullptr && L_out != nullptr);
    const size_t n = zs.size();
    CHECK_GE(n, static_cast<size_t>(2));
    CHECK_EQ(taus.size(), n);
    CHECK_EQ(eds.size(), n);
    // T_total 只取决于总光学深度（τ 可加 ⇒ 端点值即总量，降采样不改它）。
    const double T = std::exp(-taus.back());
    Vec3f S(0.0f, 0.0f, 0.0f);
    for (size_t i = 0; i + 1 < n; ++i) {
        const double dtau = taus[i + 1] - taus[i];
        const double weight = std::exp(-taus[i]) * ExpIntegralRatio(dtau);
        const Vec3f dE = eds[i + 1] - eds[i];
        S += dE * static_cast<float>(weight);
    }
    *T_total = static_cast<float>(T);
    *S_total = S;
    *L_out = L_scene * static_cast<float>(T) + S;
}

}  // namespace zdist_detail

// 精确累加器（stage 1）：控制点数不受限，段以加法叠加（乱序可加、免排序）。
//
// 用法：Reset(z_near, z_far) → 多次 AddSegment(...) → Reduce() / Integrate()。
class ZDistAccumulator {
public:
    // 重置到域 [z_near, z_far]，函数清空（τd ≡ 0、Ed ≡ 0）。
    // Pre-condition: z_far > z_near
    void Reset(double z_near, double z_far) {
        CHECK_GT(z_far, z_near) << "z_far 必须 > z_near";
        zs_ = {z_near, z_far};
        taus_.assign(2, 0.0);
        eds_.assign(2, Vec3f(0.0f, 0.0f, 0.0f));
    }

    // 叠加一个 z 段（对 τd / Ed 各加一条 ramp）。超出 [z_near,z_far] 的部分被裁掉；
    // 段与域无交叠 → 无操作。
    // Pre-condition: seg.z1 >= seg.z0；seg.sigma >= 0；seg.emission 各分量 >= 0
    void AddSegment(const ZDistSegment& seg) {
        CHECK_GE(seg.z1, seg.z0) << "z1 必须 >= z0";
        CHECK_GE(seg.sigma, 0.0) << "sigma 必须 >= 0";
        CHECK_GE(seg.emission[0], 0.0f);
        CHECK_GE(seg.emission[1], 0.0f);
        CHECK_GE(seg.emission[2], 0.0f);
        CHECK_GE(zs_.size(), static_cast<size_t>(2)) << "请先 Reset";
        const double z0 = std::max(seg.z0, zs_.front());
        const double z1 = std::min(seg.z1, zs_.back());
        if (z1 <= z0) {
            return;  // 段与域无交叠（或退化）
        }
        const double len = z1 - z0;
        const double rise_tau = seg.sigma * len;
        const Vec3f rise_ed = seg.emission * static_cast<float>(seg.sigma * len);
        // 先把两个端点变成断点（用**当前**函数值），再对所有断点加 ramp。
        InsertBreakpoint(z0);
        InsertBreakpoint(z1);
        for (size_t i = 0; i < zs_.size(); ++i) {
            const double f = zdist_detail::RampFactor(zs_[i], z0, z1);
            taus_[i] += f * rise_tau;
            eds_[i] += rise_ed * static_cast<float>(f);
        }
    }

    double z_near() const { return zs_.front(); }
    double z_far() const { return zs_.back(); }
    int size() const { return static_cast<int>(zs_.size()); }
    // Pre-condition: 0 <= i < size()
    double z(int i) const {
        CHECK_GE(i, 0);
        CHECK_LT(i, size());
        return zs_[i];
    }
    double tau(int i) const {
        CHECK_GE(i, 0);
        CHECK_LT(i, size());
        return taus_[i];
    }
    Vec3f ed(int i) const {
        CHECK_GE(i, 0);
        CHECK_LT(i, size());
        return eds_[i];
    }

    // 线性插值求值（域外按端点夹断）。
    double TauAt(double z) const {
        CHECK_GE(size(), 2) << "请先 Reset";
        return Interp(taus_, z);
    }
    Vec3f EdAt(double z) const {
        CHECK_GE(size(), 2) << "请先 Reset";
        return InterpVec(eds_, z);
    }

    // 精确末端积分（累加器本身即精确分段线性）。
    void Integrate(const Vec3f& L_scene, float* T_total /*output*/,
                   Vec3f* S_total /*output*/, Vec3f* L_out /*output*/) const {
        zdist_detail::IntegratePwl(zs_, taus_, eds_, L_scene, T_total, S_total, L_out);
    }

private:
    // 保证 x 是一个断点（缺失则用当前精确函数值插入，保持升序、无重复）。
    void InsertBreakpoint(double x) {
        const std::vector<double>::iterator it =
            std::lower_bound(zs_.begin(), zs_.end(), x);
        if (it != zs_.end() && *it == x) {
            return;
        }
        const double cur_tau = TauAt(x);
        const Vec3f cur_ed = EdAt(x);
        const size_t pos = static_cast<size_t>(it - zs_.begin());
        zs_.insert(zs_.begin() + pos, x);
        taus_.insert(taus_.begin() + pos, cur_tau);
        eds_.insert(eds_.begin() + pos, cur_ed);
    }

    double Interp(const std::vector<double>& ys, double z) const {
        if (z <= zs_.front()) {
            return ys.front();
        }
        if (z >= zs_.back()) {
            return ys.back();
        }
        const std::vector<double>::const_iterator it =
            std::upper_bound(zs_.begin(), zs_.end(), z);
        const size_t i = static_cast<size_t>(it - zs_.begin()) - 1;  // 左端点下标
        const double t = (z - zs_[i]) / (zs_[i + 1] - zs_[i]);
        return ys[i] + (ys[i + 1] - ys[i]) * t;
    }
    Vec3f InterpVec(const std::vector<Vec3f>& ys, double z) const {
        if (z <= zs_.front()) {
            return ys.front();
        }
        if (z >= zs_.back()) {
            return ys.back();
        }
        const std::vector<double>::const_iterator it =
            std::upper_bound(zs_.begin(), zs_.end(), z);
        const size_t i = static_cast<size_t>(it - zs_.begin()) - 1;
        const double t = (z - zs_[i]) / (zs_[i + 1] - zs_[i]);
        return ys[i] + (ys[i + 1] - ys[i]) * static_cast<float>(t);
    }

    std::vector<double> zs_;
    std::vector<double> taus_;
    std::vector<Vec3f> eds_;
};

// 压缩后的 ZDist（stage 2）：≤8 控制点；四条通道（τd、Ed.r/g/b）共享同一组 z 断点，
// 每条通道 = 一个 SizeLimitedPiecewiseLinearFunction<8>。
class ZDistFunction {
public:
    // 从精确累加器降采样到 ≤8 控制点。端点（z_near/z_far）恒保留 ⇒
    // 总上升量（τ_total 与 Ed_total）守恒。误差度量 = 以 exp(−τd) 加权的各通道 L2 差
    // （权重即「该处对末端源积分 S_total 的贡献」），用动态规划取全局最优。
    static ZDistFunction Reduce(const ZDistAccumulator& acc);

    // 由原始采样构造（zs 严格递增，size ∈ [2, 8]）。主要供纹理解包/测试使用。
    static ZDistFunction FromSamples(const double* zs, const double* taus,
                                     const Vec3f* eds, int n);

    // ── 降维误差度量（public，便于单测/评估）──
    // 候选点 i 的通道取值：c=0 → τd；c=1/2/3 → Ed.r/g/b。
    static double ChannelValue(const ZDistAccumulator& acc, int i, int c) {
        return (c == 0) ? acc.tau(i) : static_cast<double>(acc.ed(i)[c - 1]);
    }
    // 保留候选点 p、q 而丢掉中间点时，[z_p,z_q] 段被一条**弦**（连 (z_p,值) 与 (z_q,值)
    // 的直线）近似；本函数 = 弦与真折线之间以 exp(−τd) 加权的四通道 L2 误差（闭式）。
    static double SegmentCost(const ZDistAccumulator& acc, int p, int q) {
        const double zp = acc.z(p);
        const double zq = acc.z(q);
        double total = 0.0;
        for (int c = 0; c < 4; ++c) {
            const double yp = ChannelValue(acc, p, c);
            const double yq = ChannelValue(acc, q, c);
            const double slope = (yq - yp) / (zq - zp);
            for (int i = p; i < q; ++i) {
                const double a = acc.z(i);
                const double b = acc.z(i + 1);
                const double d0 = (yp + slope * (a - zp)) - ChannelValue(acc, i, c);
                const double d1 = (yp + slope * (b - zp)) - ChannelValue(acc, i + 1, c);
                // 权重 ≈ 子区间中点的 exp(−τd)：越近（τ 小）贡献越大。
                const double w = std::exp(-0.5 * (acc.tau(i) + acc.tau(i + 1)));
                total += w * (b - a) * (d0 * d0 + d0 * d1 + d1 * d1) / 3.0;
            }
        }
        return total;
    }
    // 给定一组保留的候选下标（严格递增，k >= 2）的总代价。
    static double ChoiceCost(const ZDistAccumulator& acc, const int* idx, int k) {
        CHECK(idx != nullptr);
        CHECK_GE(k, 2);
        double total = 0.0;
        for (int i = 0; i + 1 < k; ++i) {
            total += SegmentCost(acc, idx[i], idx[i + 1]);
        }
        return total;
    }

    int size() const { return tau_.size(); }
    // Pre-condition: 0 <= i < size()
    double z(int i) const { return tau_.x(i); }
    double tau(int i) const { return tau_.y(i); }
    Vec3f ed(int i) const {
        return Vec3f(static_cast<float>(ed_r_.y(i)),
                     static_cast<float>(ed_g_.y(i)),
                     static_cast<float>(ed_b_.y(i)));
    }

    // 末端积分：T = exp(−τd(z_far))；S = ∫exp(−τd)dEd；L_out = L_scene·T + S。
    // Pre-condition: size() >= 2
    void Integrate(const Vec3f& L_scene, float* T_total /*output*/,
                   Vec3f* S_total /*output*/, Vec3f* L_out /*output*/) const {
        CHECK_GE(size(), 2);
        std::vector<double> zs;
        std::vector<double> taus;
        std::vector<Vec3f> eds;
        zs.reserve(size());
        taus.reserve(size());
        eds.reserve(size());
        for (int i = 0; i < size(); ++i) {
            zs.push_back(z(i));
            taus.push_back(tau(i));
            eds.push_back(ed(i));
        }
        zdist_detail::IntegratePwl(zs, taus, eds, L_scene, T_total, S_total, L_out);
    }

private:
    void AddPoint(double z, double tau, const Vec3f& ed) {
        tau_.AddSample(z, tau);
        ed_r_.AddSample(z, static_cast<double>(ed[0]));
        ed_g_.AddSample(z, static_cast<double>(ed[1]));
        ed_b_.AddSample(z, static_cast<double>(ed[2]));
    }

    geom::math::SizeLimitedPiecewiseLinearFunction<kZDistControlPoints> tau_;
    geom::math::SizeLimitedPiecewiseLinearFunction<kZDistControlPoints> ed_r_;
    geom::math::SizeLimitedPiecewiseLinearFunction<kZDistControlPoints> ed_g_;
    geom::math::SizeLimitedPiecewiseLinearFunction<kZDistControlPoints> ed_b_;
};

inline ZDistFunction ZDistFunction::FromSamples(const double* zs,
                                                const double* taus,
                                                const Vec3f* eds, int n) {
    CHECK(zs != nullptr && taus != nullptr && eds != nullptr);
    CHECK_GE(n, 2);
    CHECK_LE(n, kZDistControlPoints);
    ZDistFunction out;
    for (int i = 0; i < n; ++i) {
        out.AddPoint(zs[i], taus[i], eds[i]);
    }
    return out;
}

inline ZDistFunction ZDistFunction::Reduce(const ZDistAccumulator& acc) {
    const int m = acc.size();
    CHECK_GE(m, 2);
    if (m <= kZDistControlPoints) {
        ZDistFunction out;
        for (int i = 0; i < m; ++i) {
            out.AddPoint(acc.z(i), acc.tau(i), acc.ed(i));
        }
        return out;
    }

    const int K = kZDistControlPoints;  // 目标控制点数 ⇒ K-1 段
    const double kInf = 1e300;
    // dp[s][q] = 用 s 段覆盖候选 [0..q]、末顶点落在 q 的最小代价（s ∈ [1, K-1]）。
    // 选点数 = s+1；答案 = dp[K-1][m-1]（共 K 个点）。prev[s][q] 记录前驱顶点。
    std::vector<std::vector<double>> dp(K, std::vector<double>(m, kInf));
    std::vector<std::vector<int>> prev(K, std::vector<int>(m, -1));
    for (int q = 1; q < m; ++q) {
        dp[1][q] = SegmentCost(acc, 0, q);  // 一段：顶点 0 与 q
        prev[1][q] = 0;
    }
    for (int s = 2; s <= K - 1; ++s) {
        for (int q = s; q < m; ++q) {  // q >= s：至少要 s 个顶点才排得下 s 段
            for (int p = s - 1; p < q; ++p) {
                if (dp[s - 1][p] >= kInf) {
                    continue;
                }
                const double v = dp[s - 1][p] + SegmentCost(acc, p, q);
                if (v < dp[s][q]) {
                    dp[s][q] = v;
                    prev[s][q] = p;
                }
            }
        }
    }
    // 回溯：末顶点 m-1 起，沿 prev 回到顶点 0。
    std::vector<int> chosen(K, 0);
    int idx = m - 1;
    chosen[K - 1] = idx;
    for (int s = K - 1; s >= 1; --s) {
        idx = prev[s][idx];
        chosen[s - 1] = idx;
    }
    CHECK_EQ(chosen[0], 0);
    CHECK_EQ(chosen[K - 1], m - 1);

    ZDistFunction out;
    for (int k = 0; k < K; ++k) {
        const int i = chosen[k];
        out.AddPoint(acc.z(i), acc.tau(i), acc.ed(i));
    }
    return out;
}

}  // namespace jpov

#endif  // JPOV_SRC_FIRE_FOG_ZDIST_FUNCTION_H_
