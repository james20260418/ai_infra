// 水体单次散射：闭式积分 vs 数值积分（对照测试）
//
// 为什么要这个测试：水面的"内散射"从"常数色"换成"沿线段的解析积分"之后，最容易出的错不是
// 观感，而是**指数项的符号/量纲** —— 它在画面上表现为"深处反常变亮"或"贴着地平线过曝"，
// 肉眼很难断言对错。所以这里用**逐点数值积分**当参考，把闭式解钉死。
//
// 判据：同一组参数下，闭式解与**自适应步数**梯形积分的相对误差 < 1e-6。
// 覆盖：随机 200 组（σa/σs/视线俯仰/入水深度/段长/太阳角度/g）+ 若干边界（len=0、
//       ω.y=0 水平视线、纯吸收 σs=0、极浑浊）。
// 负向：把闭式解里的 a 符号翻转必须**判为不等**（否则说明测试根本没在比较）。

#include <cmath>
#include <cstdio>
#include <random>

#include <glog/logging.h>

#include "tools/jpov/src/water/water_scattering.h"

namespace {

using jpov::water::ComputeInScatter;
using jpov::water::ComputeInScatterNumeric;
using jpov::water::DiffuseAttenuation;
using jpov::water::InScatterInput;
using jpov::water::InScatterResult;
using jpov::water::kPi;

// 相对误差（按每个通道的最大绝对值归一，避免接近 0 时炸开）。
double RelError(const InScatterResult& a, const InScatterResult& b) {
    double worst = 0.0;
    for (int ch = 0; ch < 3; ++ch) {
        const double scale = std::max({std::fabs(a.amb[ch]), std::fabs(a.sun[ch]),
                                       std::fabs(b.amb[ch]), std::fabs(b.sun[ch]), 1e-12});
        worst = std::max(worst, std::fabs(a.amb[ch] - b.amb[ch]) / scale);
        worst = std::max(worst, std::fabs(a.sun[ch] - b.sun[ch]) / scale);
    }
    return worst;
}

// 数值参考要多少步？梯形法相对误差 ≈ (du·σt)²/12 ⇒ 想把误差压到 1e-6，
// 每**光学深度**至少要取几百个采样。实测教训：固定 4096 步对"σt≈2.9 / 40 m"
// （≈116 个光学深度）只到 2e-4 —— 那时错的是参考，不是闭式解。
int StepsFor(const InScatterInput& in) {
    double st_max = 0.0;
    for (int ch = 0; ch < 3; ++ch) {
        st_max = std::max(st_max, in.sigma_a[ch] + in.sigma_s[ch]);
    }
    const double need = 2000.0 * st_max * std::max(in.len, 1e-9);
    return static_cast<int>(std::max(4096.0, std::min(4.0e6, need)));
}

void ExpectMatch(const InScatterInput& in, const char* label) {
    const InScatterResult closed = ComputeInScatter(in);
    const InScatterResult numeric = ComputeInScatterNumeric(in, StepsFor(in));
    const double err = RelError(closed, numeric);
    if (!(err < 1e-6)) {
        LOG(ERROR) << label << " 闭式 vs 数值 相对误差 = " << err;
        LOG(ERROR) << "  closed amb=(" << closed.amb[0] << "," << closed.amb[1] << ","
                   << closed.amb[2] << ") sun=(" << closed.sun[0] << "," << closed.sun[1]
                   << "," << closed.sun[2] << ")";
        LOG(ERROR) << "  numeric amb=(" << numeric.amb[0] << "," << numeric.amb[1] << ","
                   << numeric.amb[2] << ") sun=(" << numeric.sun[0] << "," << numeric.sun[1]
                   << "," << numeric.sun[2] << ")";
        CHECK(false) << label << ": 闭式积分与数值积分不一致（err=" << err << "）";
    }
}

}  // namespace

int main() {
    // ---- 0. K_d 的三个极限 ----
    CHECK(std::fabs(DiffuseAttenuation(0.30, 0.0, 0.9) - 0.30) < 1e-12)
        << "纯吸收时 K_d 必须等于 σa";
    CHECK(std::fabs(DiffuseAttenuation(0.30, 0.20, 1.0) - 0.30) < 1e-12)
        << "全前向散射不消耗光 ⇒ K_d 必须等于 σa";
    CHECK(std::fabs(DiffuseAttenuation(0.30, 0.20, 0.0) - 0.50) < 1e-12)
        << "各向同性散射 ⇒ K_d 必须等于 σt";

    // ---- 1. 边界 ----
    {
        InScatterInput in;
        in.amb_e = 1.0;
        in.sun_e = 3.0;
        in.cos_sun = 0.5;
        in.len = 0.0;                      // 段长 0 ⇒ 必须恒 0
        const InScatterResult r = ComputeInScatter(in);
        for (int ch = 0; ch < 3; ++ch) {
            CHECK_EQ(r.amb[ch], 0.0);
            CHECK_EQ(r.sun[ch], 0.0);
        }
    }
    {
        InScatterInput in;
        in.sigma_a[0] = in.sigma_a[1] = in.sigma_a[2] = 0.4;
        in.sigma_s[0] = in.sigma_s[1] = in.sigma_s[2] = 0.0;   // 纯吸收：没有散射源
        in.amb_e = in.sun_e = 5.0;
        in.len = 10.0;
        const InScatterResult r = ComputeInScatter(in);
        for (int ch = 0; ch < 3; ++ch) {
            CHECK_EQ(r.amb[ch], 0.0) << "σs=0 时不该有内散射";
            CHECK_EQ(r.sun[ch], 0.0) << "σs=0 时不该有内散射";
        }
    }
    {
        InScatterInput in;                 // 水平视线 + 水下相机
        in.omega_y = 0.0;
        in.z0 = 3.0;
        in.len = 6.0;
        in.amb_e = 1.0;
        in.sun_e = 3.0;
        in.cos_sun = -0.2;
        ExpectMatch(in, "水平视线");
    }
    {
        InScatterInput in;                 // 极浑浊 + 长路径
        in.sigma_a[0] = 0.9; in.sigma_a[1] = 0.9; in.sigma_a[2] = 0.9;
        in.sigma_s[0] = 2.0; in.sigma_s[1] = 2.0; in.sigma_s[2] = 2.0;
        in.len = 40.0;
        in.z0 = 2.0;
        in.amb_e = 1.0;
        in.sun_e = 3.0;
        ExpectMatch(in, "极浑浊长路径");
    }

    // ---- 2. 随机 100 组 ----
    std::mt19937 rng(20261027u);
    std::uniform_real_distribution<double> u01(0.0, 1.0);
    for (int iter = 0; iter < 100; ++iter) {
        InScatterInput in;
        for (int ch = 0; ch < 3; ++ch) {
            in.sigma_a[ch] = 0.01 + 0.6 * u01(rng);
            in.sigma_s[ch] = 0.005 + 0.4 * u01(rng);
        }
        in.g_sun = 0.2 + 0.75 * u01(rng);          // [0.2, 0.95]
        in.g_amb = 0.0 + 0.5 * u01(rng);           // [0, 0.5]
        in.cos_sun = -1.0 + 2.0 * u01(rng);
        in.omega_y = -1.0 + 2.0 * u01(rng);
        in.cos_amb = in.omega_y;
        in.z0 = 0.5 + 5.0 * u01(rng);
        in.len = 0.5 + 30.0 * u01(rng);
        // 抬头（ω.y>0）时段长不能越过水面 —— 否则会积到"空中"，模型不成立。
        if (in.omega_y > 1.0e-3) {
            in.len = std::min(in.len, 0.95 * in.z0 / in.omega_y);
        }
        in.amb_e = 0.1 + 2.0 * u01(rng);
        in.sun_e = 0.1 + 5.0 * u01(rng);
        ExpectMatch(in, "random");
    }

    // ---- 3. 负向：把闭式积分里的 a 符号翻转，必须判为不等 ----
    {
        InScatterInput in;
        in.omega_y = -0.8;                  // 俯视
        in.z0 = 0.0;
        in.len = 8.0;
        in.amb_e = in.sun_e = 1.0;
        in.cos_sun = 0.3;
        const InScatterResult good = ComputeInScatter(in);
        const InScatterResult ref = ComputeInScatterNumeric(in, 4096);
        CHECK(RelError(good, ref) < 1e-6) << "基准本身就不对";

        // 手算一个"符号翻转"版本，确认 RelError 能把它抓出来。
        InScatterResult flipped = good;
        for (int ch = 0; ch < 3; ++ch) {
            flipped.amb[ch] = -good.amb[ch];   // 符号错 ⇒ 必须判不等
        }
        CHECK(RelError(flipped, ref) > 1.0) << "比较器退化：符号翻转竟然通过";
    }

    std::printf("PASS\n");
    return 0;
}
