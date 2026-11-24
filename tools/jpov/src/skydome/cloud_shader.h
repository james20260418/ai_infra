// JPOV 解析云（预实验）—— 天空球上的**纯解析**云
//
// 位置：只进 sky shader 一处（`SkyRenderer::kSkyFs` 在 `#version` 后拼进本文件的
// kCloudGlsl，再用 main() 里两处调用）。object3d / water / fog / SSR 全部不动。
//
// 为什么是「天球上的解析云」：
//   · 云是**方向函数** `d(dir)`（不是世界空间体积），所以相机平移不产生视差 ——
//     正合「不追求相机飞到云上」，也不需要求交/求交缓存/额外 pass。
//   · 云只影响背景像素 + 环境探针（同一个 shader 画 cube），镜面反射里的云自动一致。
//   · 太阳遮挡 = **alpha over**（自然）+ 对高动态范围光源（日盘 2e5）额外一个指数
//     （见 CloudLightBlock）；不需要任何 shadow map / CSM。
//
// 五个方案（预实验对比用）：
//   1 kPlaneFbm  —— 平面投影 + 4 octave fbm：层积云/碎积云（最便宜）
//   2 kWorley    —— Worley F1 团块 + fbm 细节：积云/积雨云（团块感）
//   3 kCirrus    —— 各向异性拉伸 fbm：高空卷云/丝状云（夕阳最好看）
//   4 kPlanes    —— 多层视差云板（3 层）：有遮挡/视差（真·体积步进试过，见实现注释）
//   5 kLayered   —— 卷云叠在积云上（两层，观感最「真」，成本 = 1+3）
//
// 与 CPU 同源：`CoverageAtDirection()` 是 GLSL 密度的**逐行移植**（同一套整数 hash），
// 供「太阳被云遮 → 直射光衰减成 ambient」这类**需要 CPU 知道云量**的推导使用
// （见 demo/skylight_scene.h）。改噪声常量时必须两边一起改。

#ifndef JPOV_SRC_SKYDOME_CLOUD_SHADER_H_
#define JPOV_SRC_SKYDOME_CLOUD_SHADER_H_

#include <cmath>
#include <cstdint>

#include "tools/jpov/interface/render_command.h"   // CloudCommand

namespace jpov {
namespace cloud {

// ─────────────────────────────────────────────────────────────────────────────
// GLSL：拼接进 sky FS 的 `#version` 行之后（语法上等价于文件顶部的全局区）。
// ⚠️ 这里**只声明云自己的 uniform**，相机位置等由 main() 以参数传入
//    （天空 shader 的 `uCamPos` 声明在拼接点之后，此处引用会编译失败）。
// ⚠️ 整数 hash 一律用十进制字面量（GLSL 330 对 0x…u 的支持依驱动而异）。
// ─────────────────────────────────────────────────────────────────────────────
inline constexpr const char* kCloudGlsl = R"glsl(
uniform int   uCloudOn;         // 0 = 关（默认，逐字节零回归）
uniform int   uCloudScheme;     // 1..5，见 jpov::CloudScheme
uniform float uCloudCoverage;   // 0..1 云量（阈值偏置）
uniform float uCloudScale;      // 云块大小（1/rad 量级）
uniform float uCloudOpacity;    // 云最厚处不透明度
uniform float uCloudSoftness;   // 边缘过渡宽度
uniform vec2  uCloudWind;       // 确定性偏移（CPU 累积，禁用 uTime ⇒ gold 可重放）
uniform float uCloudHeight;     // 云层高度（米，预留给真·体积方案）
uniform float uCloudThickness;  // 云层厚度（米，预留给真·体积方案）
uniform float uCloudSunOcc;     // 0..1 太阳遮挡强度
uniform float uCloudBrightness; // 云亮度倍数
uniform int   uCloudStepLimit;  // >0 ⇒ 用这个步数（交互期降档）；0 = 默认 32 步
uniform int   uCloudFine;       // 1 = 像素锁定细节层开；0 = 关（降档）

// ── 整数 hash（与 CPU 完全同源；禁用 sin-hash：软件 Mesa 与真卡 sin 精度不同）──
uint cloudHash(uint x) {
    x ^= x >> 16;
    x *= 2146121005u;
    x ^= x >> 15;
    x *= 2221713035u;
    x ^= x >> 16;
    return x;
}

float cloudHash21(vec2 p) {
    ivec2 i = ivec2(floor(p));
    uint h = cloudHash(uint(i.x) * 1973u + uint(i.y) * 9277u + 26699u);
    return float(h) * (1.0 / 4294967296.0);
}

// 双线性 value noise（smoothstep 权重）
float cloudNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = cloudHash21(i);
    float b = cloudHash21(i + vec2(1.0, 0.0));
    float c = cloudHash21(i + vec2(0.0, 1.0));
    float d = cloudHash21(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// 归一化 fbm（0..1，均值 ≈0.5）。每层旋转 53° 打散轴向对齐。
float cloudFbm(vec2 p, int octaves) {
    float sum = 0.0;
    float amp = 0.5;
    float norm = 0.0;
    const mat2 rot = mat2(0.601815, -0.798635, 0.798635, 0.601815);
    for (int i = 0; i < octaves; ++i) {
        sum += amp * cloudNoise(p);
        norm += amp;
        p = rot * p * 2.02;
        amp *= 0.5;
    }
    return norm > 0.0 ? sum / norm : 0.0;
}

// ── 带限（LOD）fbm ───────────────────────────────────────────────────────────
// 解析噪声的「分辨率」并不是无限的：第 i 个 octave 的胞元尺寸是 1/2^i（采样空间）。
// 当**一个像素跨过的距离** fp 超过半个胞元（Nyquist），该 octave 采出来就是白噪声
// —— 这正是「噪点」和「分辨率突然不行」的来源（近地平线时一像素跨几百米，
// 150 m 的第 4 octave 直接变成雪花）。
//
// 做法：按 fp 逐 octave 求权重 w（fp*freq 越过 0.25 开始淡出，0.5 完全丢弃），
// **丢弃的 octave 用它的均值 0.5 代替**。归一化 fbm 的均值恒为 0.5 ⇒ 「丢细节」
// 既不变暗也不变薄，只变平滑。退化因此是连续的：不会突然糊、也不会突然噪。
const float kCloudLodStart = 0.25;   // 每个胞元至少 4 个像素时开始淡出
const float kCloudLodEnd   = 0.50;   // 每胞元 2 像素 = Nyquist，完全丢弃

// 某个 octave 的权重：1 = 完全采样，0 = 低于 Nyquist（只保留均值）
float cloudLodWeight(float fp, float freq) {
    return clamp((kCloudLodEnd - fp * freq) / (kCloudLodEnd - kCloudLodStart),
                 0.0, 1.0);
}

// 被保留的细节**幅度占比**（0..1）。用于把「细节丢了多少」传给阈值过渡宽度：
// 细节全丢时覆盖率阈值必须变软，否则近处有云的像素和远处糊成一片的像素之间
// 会出现一条硬边（那就是「分辨率突然不行」的可见形态）。
float cloudLodKeep(float fp, int octaves) {
    float norm = 1.0 - exp2(-float(octaves));
    float kept = 0.0;
    float amp = 0.5;
    float freq = 1.0;
    for (int i = 0; i < octaves; ++i) {
        float w = cloudLodWeight(fp, freq);
        if (w <= 0.0) {
            break;
        }
        kept += amp * w;
        amp *= 0.5;
        freq *= 2.0;
    }
    return norm > 0.0 ? kept / norm : 0.0;
}

// 2D 带限 fbm（每层旋转 53°、×2.02，与 cloudFbm 同构）
float cloudFbmLod(vec2 p, int octaves, float fp) {
    float norm = 1.0 - exp2(-float(octaves));
    const mat2 rot = mat2(0.601815, -0.798635, 0.798635, 0.601815);
    float sum = 0.0;
    float amp = 0.5;
    float freq = 1.0;
    for (int i = 0; i < octaves; ++i) {
        float w = cloudLodWeight(fp, freq);
        if (w <= 0.0) {
            break;
        }
        sum += amp * w * (cloudNoise(p) - 0.5);
        p = rot * p * 2.02;
        amp *= 0.5;
        freq *= 2.02;
    }
    return 0.5 + sum / norm;
}

// Worley F1（返回最近特征点的距离，0..~1.4）
float cloudWorley(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    float best = 8.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 g = vec2(float(x), float(y));
            vec2 o = vec2(cloudHash21(i + g + vec2(11.0, 5.0)),
                          cloudHash21(i + g + vec2(37.0, 91.0)));
            vec2 r = g + o - f;
            best = min(best, dot(r, r));
        }
    }
    return sqrt(best);
}

// 天球 → 2D 云域：**球极平面投影**（stereographic，从天顶投影到下切平面）。
//
//     p = (x, z) / (1 + y)        天顶 → 0，地平线 → 半径 1
//
// 为什么不用常见的 `dir.xz / dir.y`（平面云板）：那个映射在 dir.y→0 时频率发散，
// 像素采样必然退化成"径向拉丝/条纹"（实测就是地平线上一圈栅栏）；在天顶又收缩到
// 一点，形成放射状串珠。球极投影是**共形映射**（保角），只在尺度上压缩
// （地平线处角频率只比天顶高 2 倍，而不是无穷）⇒ 云块形状不被拉伸、地平线自然汇聚。
vec2 cloudPlane(vec3 dir) {
    return dir.xz / (1.0 + max(dir.y, -0.9));
}

// 地平线淡出：平面投影 p = dir.xz/dir.y 在 dir.y→0 时频率趋于无穷，
// 像素采样下必然退化成"径向拉丝"。真实天空里远处云也是被霾吃掉
// ⇒ 用一条 fade 把最后几度收掉，比任何反走样都便宜且更像照片。
float cloudHorizonFade(vec3 dir) {
    return smoothstep(0.008, 0.075, dir.y);
}

// 云量 → 覆盖（阈值 + softness 的 smoothstep）
float cloudCover(float n) {
    float edge = 1.0 - uCloudCoverage;
    return smoothstep(edge, edge + max(uCloudSoftness, 0.001), n);
}

// ── 三个密度场（都在 2D 平面域里）──
// 层积云/碎积云：fbm 阈值
float densityCumulus(vec2 p) {
    // 大尺度云量起伏（2 octave，很便宜）：让天空有"多云区 / 晴空区"，
    // 否则阈值化后的 fbm 会像均匀撒了一把米粒。
    float patch = cloudFbm(p * 0.22 + vec2(19.1, 3.3), 2);
    float n = cloudFbm(p, 4);
    return cloudCover(mix(n, patch, 0.28));
}
// 卷云：沿一个方向拉长 6:1，再叠一层横向细丝
float densityCirrus(vec2 p) {
    // 域扭曲：真实卷云是"被风剪切的丝"，方向大体一致但会弯、会分叉；
    // 不扭曲时就是一把笔直的刷子，程序感极强。
    vec2 warp = vec2(cloudFbm(p * 0.30 + vec2(5.1, 23.7), 2),
                     cloudFbm(p * 0.30 + vec2(37.3, 2.9), 2)) * 1.6 - 0.8;
    vec2 q = vec2((p.x + warp.x) * 0.42, (p.y + warp.y) * 1.55);
    float a = cloudFbm(q, 3);
    float b = cloudFbm(q * 3.1 + vec2(17.3, 9.1), 2);
    return cloudCover(a * 0.74 + b * 0.26) * 0.72;
}
// 积云：Worley 团块（1-F1）+ fbm 细节
float densityWorley(vec2 p) {
    // 域扭曲：把规则胞元打散，否则透视压缩下会出现"一排排圆饼"的网格感
    vec2 warp = vec2(cloudFbm(p * 0.22 + vec2(11.3, 4.1), 2),
                     cloudFbm(p * 0.22 + vec2(41.7, 27.9), 2)) * 1.0 - 0.5;
    float w = 1.0 - cloudWorley((p + warp) * 0.55);
    float det = cloudFbm(p * 1.8 + vec2(3.7, 1.9), 3);
    // 大尺度云量起伏：让天空有"多云区/晴空区"，而不是均匀撒点
    float patch = cloudFbm(p * 0.22 + vec2(71.3, 13.7), 2);
    return cloudCover(w * 0.62 + det * 0.18 + patch * 0.20);
}
// 按层取密度：0 = 积云（fbm）、1 = 卷云、2 = Worley 团块
float densityLayer(vec2 p, int layer) {
    if (layer == 1) return densityCirrus(p);
    if (layer == 2) return densityWorley(p);
    return densityCumulus(p);
}

// 方案主密度（kPlanes 的每一层都用它）
float densityMain(vec2 p) {
    if (uCloudScheme == 2) return densityWorley(p);
    if (uCloudScheme == 3) return densityCirrus(p);
    return densityCumulus(p);
}

float cloudDensity(vec3 dir) {
    return densityMain(cloudPlane(dir) * uCloudScale + uCloudWind);
}

// 云体受光。
//
// 关键：只用「朝光源偏移一次」的密度（自阴影）时，明暗差完全由**水平方位**决定，
// 而真实云的受光主要由**高度**决定（光从上面来）—— 于是会出现"云顶该亮却暗、
// 云底该暗却亮"的倒错观感（实测被一眼看出）。
//
// 这里用密度的 2.5D 梯度造一个"云团法线"：n = normalize(-dp/dx, 1, -dp/dz)，
// 再用 N·L 给明暗。代价是 4 次额外密度采样，换来的是真的"受光面/背光面"。
vec3 cloudBody(vec2 p, float d, vec3 bg, vec3 lightCol, vec3 lightDir, int layer) {
    // 云团法线 = 密度场的 2.5D 高度场梯度：n = normalize(-∂d/∂x, 1, -∂d/∂z)。
    // 只用 N·L 给明暗 —— **不要**再用「朝光源偏移一次采样」当自阴影：
    // 那个量近似等于"我是不是在云的边缘"（边缘朝哪都低）⇒ 每朵云都变成
    // "黑心 + 亮边"，正午看起来就是"上暗下亮"的倒错（实测被一眼看穿）。
    const float e = 0.10;
    float gx = densityLayer(p + vec2(e, 0.0), layer) -
               densityLayer(p - vec2(e, 0.0), layer);
    float gz = densityLayer(p + vec2(0.0, e), layer) -
               densityLayer(p - vec2(0.0, e), layer);
    // 符号约定：从下往上看云（相机在云层下方），屏幕上方 = 更靠近太阳的那一侧，
    // 应该更亮。用 +grad 而不是 -grad（两者只差"朝光面往哪边倒"，实测 + 才对）。
    vec3 n = normalize(vec3(gx * 3.0, 1.0, gz * 3.0));
    float ndl = clamp(dot(n, normalize(lightDir)), 0.0, 1.0);

    // 环境项用 bg（与天色同源）但**去饱和**：云的散射接近中性白。
    float bg_lum = dot(bg, vec3(0.2126, 0.7152, 0.0722));
    vec3 amb = mix(bg, vec3(bg_lum), 0.62);
    vec3 body = amb * 0.60 + lightCol * (0.05 + 1.30 * ndl);
    // 厚处明显压暗 ⇒ 读起来像"云体的内部"，而不是一张均匀的贴片。
    body *= mix(0.76, 1.0, 1.0 - d);
    // 注意：这里**不加**"沿轮廓的银边"项 —— 实测它会让每朵云变成
    // "白圈包灰心"的煎蛋（夕阳时尤其明显）。逆光的银边由 cloudSample 里的
    // 前向散射项（pow(dot(view,light),14)）负责，那才是物理来源。
    return body * uCloudBrightness;
}

// 云对**发光天体**（日盘/月盘）的遮挡系数。
// 盘亮度 ~2e5 × 天空，和云的 radiance 差 5 个数量级 ⇒ 只靠 alpha over 时
// α=0.9 仍会透出 2e4 的盘（"云盖不住太阳"）。故对盘**另加一个强指数**：
// 同样的密度，τ 乘 14 后再取 exp。
float cloudLightBlock(vec3 lightDir) {
    if (uCloudOn == 0) return 1.0;
    float d = cloudDensity(lightDir) * uCloudOpacity;
    return mix(1.0, exp(-d * 14.0), clamp(uCloudSunOcc, 0.0, 1.0));
}


// ═══════════════════════════════════════════════════════════════════════════
// 方案 6：**真 3D 密度场 + 固定高度层 + 光线步进**
//
// 为什么前 5 个方案都像贴纸：它们把云当成**方向函数** d(dir)，等价于在球面上贴
// 一张 2D 图 —— 上半球每个方向只有一个采样，**没有任何垂直结构**，所以既不"上下
// 有厚度"，也不会随视线俯仰产生视差。业界（Nubis / Hillaire 2020 / 各家引擎）的
// 做法都是：
//   ① 密度场是 **3D**：d(x,y,z)；
//   ② 云层是**固定的世界高度区间** [BaseAltitude, BaseAltitude+Thickness]，
//      不是"相机上方的偏移层"；
//   ③ 垂直剖面 analytic（积云 = 平底 + 圆顶），底平是"层"的最强视觉线索；
//   ④ 沿视线步进积分（front-to-back），沿光方向再步进求自阴影；
//   ⑤ 高度切变（wind shear）：云的上下截面错开，视觉上立刻"立体"。
//
// 本文件里全部用**解析噪声**实现（不用任何纹理）：3D 值噪声 + fbm。
// 代价：每像素 ~12 步 × (3 octave 3D + 2 次光程采样)，llvmpipe 上约 0.5–1 s/帧 ——
// 静止图完全可接受，实时交互要降分辨率（JPOV 后面可以接半分辨率 + 上采样）。
// ═══════════════════════════════════════════════════════════════════════════

// 周期化格点 hash：p 按 period 环绕 ⇒ 噪声场在**世界空间以 period 个胞元为周期**，
// 于是 `wind_offset` 加满 period 时云场逐像素回到原状 ⇒ 可以**无缝循环**播放。
// （用 floor 除法而不是 % —— GLSL 的 % 对负数会给负数。）
uint cloudHash3p(ivec3 p, int period) {
    ivec3 q = p - period * ivec3(floor(vec3(p) / float(period)));
    uint h = uint(q.x) * 1973u ^ uint(q.y) * 9277u ^ uint(q.z) * 4177u;
    return cloudHash(h + 26699u);
}

uint cloudHash3(ivec3 p) {
    uint h = uint(p.x) * 1973u ^ uint(p.y) * 9277u ^ uint(p.z) * 4177u;
    return cloudHash(h + 26699u);
}

// 已经折回 [0,period) 的格点 + ±1 的邻居偏移 ⇒ 只需要一次减法折回，不做除法。
// Pre-condition: 0 <= c < period + 2（由 cloudNoise3p 保证）
uint cloudHash3c(ivec3 c, int period) {
    ivec3 w = ivec3(c.x >= period ? c.x - period : c.x,
                    c.y >= period ? c.y - period : c.y,
                    c.z >= period ? c.z - period : c.z);
    uint h = uint(w.x) * 1973u ^ uint(w.y) * 9277u ^ uint(w.z) * 4177u;
    return cloudHash(h + 26699u);
}

// 3D 值噪声（可平铺版）：每个 octave 的 period 翻倍（与 ×2 的 lacunarity 配套）
//
// ⚠️ 性能要害：原来每个角点都走 cloudHash3p ⇒ **每噪声 8×3 = 24 次浮点除法**。
// 一个密度采样有 ~40 次噪声 ⇒ ~1000 次除法，是整条云管线最大的单点开销。
// 现在改成：入口做**一次**取模把域折回 [0,period)，8 个角点只用整数比较折回。
float cloudNoise3p(vec3 p, int period) {
    float pf = float(period);
    p -= pf * floor(p / pf);
    vec3 i = floor(p);
    vec3 f = p - i;
    vec3 u = f * f * (3.0 - 2.0 * f);
    ivec3 c = ivec3(i);
    const float kInv = 1.0 / 4294967296.0;
    float n000 = float(cloudHash3c(c, period)) * kInv;
    float n100 = float(cloudHash3c(c + ivec3(1, 0, 0), period)) * kInv;
    float n010 = float(cloudHash3c(c + ivec3(0, 1, 0), period)) * kInv;
    float n110 = float(cloudHash3c(c + ivec3(1, 1, 0), period)) * kInv;
    float n001 = float(cloudHash3c(c + ivec3(0, 0, 1), period)) * kInv;
    float n101 = float(cloudHash3c(c + ivec3(1, 0, 1), period)) * kInv;
    float n011 = float(cloudHash3c(c + ivec3(0, 1, 1), period)) * kInv;
    float n111 = float(cloudHash3c(c + ivec3(1, 1, 1), period)) * kInv;
    return mix(mix(mix(n000, n100, u.x), mix(n010, n110, u.x), u.y),
               mix(mix(n001, n101, u.x), mix(n011, n111, u.x), u.y), u.z);
}

// 3D 值噪声（8 角点三线性 + smoothstep 权重）
float cloudNoise3(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    vec3 u = f * f * (3.0 - 2.0 * f);
    ivec3 c = ivec3(i);
    const float kInv = 1.0 / 4294967296.0;
    float n000 = float(cloudHash3(c)) * kInv;
    float n100 = float(cloudHash3(c + ivec3(1, 0, 0))) * kInv;
    float n010 = float(cloudHash3(c + ivec3(0, 1, 0))) * kInv;
    float n110 = float(cloudHash3(c + ivec3(1, 1, 0))) * kInv;
    float n001 = float(cloudHash3(c + ivec3(0, 0, 1))) * kInv;
    float n101 = float(cloudHash3(c + ivec3(1, 0, 1))) * kInv;
    float n011 = float(cloudHash3(c + ivec3(0, 1, 1))) * kInv;
    float n111 = float(cloudHash3(c + ivec3(1, 1, 1))) * kInv;
    return mix(mix(mix(n000, n100, u.x), mix(n010, n110, u.x), u.y),
               mix(mix(n001, n101, u.x), mix(n011, n111, u.x), u.y), u.z);
}

// 3D fbm —— **可平铺版**（循环动画用）。
// 每层用**轴置换**而不是旋转：旋转会破坏格点对齐 ⇒ 周期性失效（云会跳）。
// 轴置换 + ×2 的 lacunarity 与 period 翻倍配套，场在世界空间严格周期。
float cloudFbm3p(vec3 p, int period, int octaves) {
    float sum = 0.0;
    float amp = 0.5;
    float norm = 0.0;
    for (int i = 0; i < octaves; ++i) {
        sum += amp * cloudNoise3p(p, period);
        norm += amp;
        p = p.zxy * 2.0;
        period *= 2;
        amp *= 0.5;
    }
    return norm > 0.0 ? sum / norm : 0.0;
}

// 3D fbm（每层旋转 + ×2.03，避免轴向对齐）
float cloudFbm3(vec3 p, int octaves) {
    float sum = 0.0;
    float amp = 0.5;
    float norm = 0.0;
    const mat3 rot = mat3(0.00, 0.80, 0.60,
                         -0.80, 0.36, -0.48,
                         -0.60, -0.48, 0.64);
    for (int i = 0; i < octaves; ++i) {
        sum += amp * cloudNoise3(p);
        norm += amp;
        p = rot * p * 2.03;
        amp *= 0.5;
    }
    return norm > 0.0 ? sum / norm : 0.0;
}

// 3D 带限 fbm（与 cloudFbm3p 同构：轴置换 + period 翻倍，保持可平铺）。
// fp 的单位是**采样空间的胞元**（拍像素跨过多少个基频胞元），由 marching 的步长与
// 像素足迹算出来（见 marchCloudLayer）。
float cloudFbm3pLod(vec3 p, int period, int octaves, float fp) {
    float norm = 1.0 - exp2(-float(octaves));
    float sum = 0.0;
    float amp = 0.5;
    float freq = 1.0;
    for (int i = 0; i < octaves; ++i) {
        float w = cloudLodWeight(fp, freq);
        if (w <= 0.0) {
            break;   // 更细的 octave 全部低于像素分辨率：只剩均值，不必再算
        }
        sum += amp * w * (cloudNoise3p(p, period) - 0.5);
        p = p.zxy * 2.0;
        period *= 2;
        amp *= 0.5;
        freq *= 2.0;
    }
    return 0.5 + sum / norm;
}

// ── 云域常数（CPU 侧必须一致）──
// 特征尺度 1200 m：850 m 时云"太矮太胖"，观感上整体偏低（Danis 反馈）。
// 噪声周期 16 个胞元 = 19.2 km 的漂移行程 ⇒ 一圈循环云场逐像素回到原状。
const float kCloudFeatureM   = 1200.0;
// 大气透视的特征距离（米）：t/40km 处约 63% 被洗成天光色。
const float kAerialPerspectiveM = 40000.0;
// 最远端的最大混合比例（1.0 = 远处完全等于天光色 ⇒ 云会消失；留 0.75 保住"云还在"）。
const float kAerialMaxMix = 0.75;
const int   kCloudNoisePeriod = 16;

// 3D Worley F1（可平铺）：每格只取 **1 个** hash，再解包成 3 个字节当特征点偏移
// —— 27 个邻居格 = 27 次 hash（若每格取 3 次就是 81 次，太贵）。
float cloudWorley3p(vec3 p, int period) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    float best = 8.0;
    const float kInv = 1.0 / 255.0;
    for (int z = -1; z <= 1; ++z) {
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                ivec3 c = ivec3(i) + ivec3(x, y, z);
                uint h = cloudHash3p(c, period);
                vec3 o = vec3(float(h & 255u), float((h >> 8) & 255u),
                              float((h >> 16) & 255u)) * kInv;
                vec3 r = vec3(float(x), float(y), float(z)) + o - f;
                best = min(best, dot(r, r));
            }
        }
    }
    return sqrt(best);
}


// 直射光穿过大气的**颜色**衰减（云专用）。
//
// 低仰角时大气质量按 1/sin(elev) 暴增（7° ≈ 8.2 倍），蓝光被瑞利散射吃掉 ⇒
// 云才会"烧"成橙红（照片里的火烧云就是这个）。现有 sun_body_color 只是**黑体色温**
// （2000K→5600K 线性），在 7° 时还有 ~3460K（淡黄），远不够红。
//
// ⚠️ 只作用于云：日盘/天光共用的色温曲线是全项目单点，动它会改所有 gold。
//    统一口径的时机 = 天空色温曲线也换成 airmass 模型时（记在 known_issue）。
vec3 cloudSunExtinction(vec3 lightDir) {
    float airmass = 1.0 / max(lightDir.y, 0.02);
    // 系数取可见光三通道的差异衰减：7° 时 ≈ (0.61, 0.27, 0.045) —— 蓝几乎没了。
    return exp(-vec3(0.075, 0.190, 0.460) * max(airmass - 1.0, 0.0));
}

// ── 像素锁定的细节层（「细腻」的唯一来源）─────────────────────────────────────
//
// 普通 fbm 的高频层振幅按 ×0.5 衰减：第 7 层只剩 1/128，等于不存在 —— 所以
// 「多加 octave」并不会让云变细腻（实测 5 层与 7 层逐像素几乎一致）。
//
// 这里换一种做法：**细节的胞元尺寸锁定在屏幕上约 kDetailPixels 个像素**，振幅不衰减。
// 于是任何距离下看到的细节颗粒度都一样（近处是真细节，远处自动变粗，永远是 5 px），
// 也就不会出现「细节比像素还小 ⇒ 退化成噪点 / 突然糊掉」。
//
// 频率必须是 2 的整数次幂（相邻两层交叉淡化）——这样云场仍然严格可平铺，
// 循环动画「t=一圈」逐像素回到原状的硬指标不会被破坏。
const float kDetailPixels = 7.0;   // 细节胞元 ≈ 7 个屏幕像素（单层 5 px 读作"磨砂膜"，7 px + 交叉淡化才像绒毛）

// q 已在 feature_m 单位下；fp = 每像素跨过的 q 距离（胞元/像素）
float cloudDetailSelfSimilar(vec3 q, int period, float fp) {
    // 目标胞元（q 单位）= 5 px × 每像素跨过的距离
    float target = max(kDetailPixels * fp, 1.0e-9);
    float lvl = log2(1.0 / target);                 // 需要的频率层（可为负）
    lvl = clamp(lvl, 0.0, 10.0);
    float l0 = floor(lvl);
    float fr = lvl - l0;
    float f0 = exp2(l0);
    float f1 = f0 * 2.0;
    float n0 = cloudNoise3p(q * f0, period * int(f0 + 0.5));
    float n1 = cloudNoise3p(q * f1, period * int(f1 + 0.5));
    // 只花 2 次采样（相邻两层交叉淡化）。曾试过再叠一层粗一倍的第三层（读成"绒毛"
    // 略好一点），但那是本函数里 1/3 的着色成本，性价比不成立，已去掉。
    return mix(n0, n1, fr);
}

// 单步密度。h = 层内归一化高度 0..1。
// fp = 像素足迹（单位：基频胞元 = feature_m）。所有噪声采样都经带限 fbm，
// 所以传进来的 fp 决定「这一像素能分辨到第几 octave」——远处自然只剩低频。
// kind: 0 = fbm 积云、1 = Worley 花椰菜、2 = 各向异性拉伸（层云/卷云）、3 = 域扭曲湍流
float cloudDensity3(vec3 pos, float h, float base_y, float thick,
                    float feature_m, float fp, int shape_oct, float detail_amp,
                    float cov_mul, int kind) {
    if (h < 0.0 || h > 1.0) return 0.0;

    vec3 q = pos / feature_m;
    q.xz += uCloudWind;                       // 主风漂移（周期化 ⇒ 无缝循环）
    q.xz += vec2(h * 0.34, h * 0.14);         // 高度切变：上下截面错开 ⇒ 看得见云的侧面

    // ① 形状（3D，可平铺）—— 换 kind 只换这一个函数，其余链路完全相同
    float base;
    if (kind == 1) {
        // Worley 花椰菜：1-F1 给"团块中心厚、边缘碎"，再叠 fbm 细节
        // Worley 的规则格点会给出"一串等大圆球"⇒ 先把域掰弯再取 F1
        vec3 ww = vec3(cloudFbm3pLod(q * 0.5 + vec3(2.3, 8.1, 4.4), kCloudNoisePeriod, 2, fp * 0.5),
                       cloudFbm3pLod(q * 0.5 + vec3(19.7, 3.3, 11.1), kCloudNoisePeriod, 2, fp * 0.5),
                       cloudFbm3pLod(q * 0.5 + vec3(6.1, 27.9, 1.7), kCloudNoisePeriod, 2, fp * 0.5)) - 0.5;
        float w = 1.0 - cloudWorley3p((q + ww * 2.2) * 0.85, kCloudNoisePeriod);
        // 过渡带加宽 + 提高 fbm 权重（纯 Worley 阈值化 = 一串等大乒乓球）
        base = cloudFbm3pLod(q, kCloudNoisePeriod, shape_oct, fp) * 0.50 +
               smoothstep(0.02, 0.98, w) * 0.50;
    } else if (kind == 2) {
        // 各向异性：水平拉长 / 垂直压扁 ⇒ 层云、卷云那种"丝缕"
        vec3 qq = vec3(q.x * 0.45, q.y * 1.9, q.z * 1.6);
        base = cloudFbm3pLod(qq, kCloudNoisePeriod, shape_oct, fp * 1.9);
    } else if (kind == 3) {
        // 域扭曲（湍流）：低频 fbm 把域掰弯 ⇒ 边缘不规则、有"翻滚"感
        vec3 warp = vec3(cloudFbm3pLod(q * 0.55 + vec3(3.1, 9.7, 1.3),
                                       kCloudNoisePeriod, 2, fp * 0.55),
                         cloudFbm3pLod(q * 0.55 + vec3(21.3, 5.1, 13.7),
                                       kCloudNoisePeriod, 2, fp * 0.55),
                         cloudFbm3pLod(q * 0.55 + vec3(7.7, 31.1, 2.9),
                                       kCloudNoisePeriod, 2, fp * 0.55)) - 0.5;
        base = cloudFbm3pLod(q + warp * 0.9, kCloudNoisePeriod, shape_oct, fp);
    } else {
        base = cloudFbm3pLod(q, kCloudNoisePeriod, shape_oct, fp);
    }

    // ② 天气图（大尺度云量分布）：同一个可平铺场，取 1/4 频率、错开的格子
    float weather = cloudFbm3pLod(q * 0.25 + vec3(13.0, 5.0, 7.0),
                                  kCloudNoisePeriod / 4, 2, fp * 0.25);
    float cov = clamp(uCloudCoverage * (0.42 + 1.15 * weather) * cov_mul, 0.0, 0.985);

    // ③ 阈值（硬边）：0.26 的软过渡会让整片天空都是"薄云"⇒ 大片发白（Danis 反馈）
    //    但**细节被像素分辨率吃掉时必须自动放宽**：否则「近处锐利 / 远处糊成一片」
    //    的交界会露出一条硬边 —— 这正是「分辨率突然不行」的可见形态。
    float detail_keep = cloudLodKeep(fp, shape_oct);
    float soft = max(uCloudSoftness, 0.001) + 0.35 * (1.0 - detail_keep);

    // 像素锁定细节必须加在**阈值之前**：它扰动的是"形状标量"，云边才会在屏幕上
    // 真的被纤毛化。加在阈值之后只会被沿视线的积分平均掉（实测 1–2px 能量只涨 10%）。
    // 均值 0（噪声 −0.5）⇒ 平均云量不变。
    if (detail_amp > 0.0 && uCloudFine != 0) {
        base += (cloudDetailSelfSimilar(q, kCloudNoisePeriod, fp) - 0.5) * detail_amp;
    }
    float d = smoothstep(1.0 - cov, 1.0 - cov + soft, base);

    // ④ 垂直剖面：底平（凝结底）+ 圆顶
    //
    // **底不能是绝对平面**：掠射时所有视线都擦过云底，一条完美的水平线会在整条
    // 地平线上被压成"纸片被刀切"的硬边（Danis 2026-11-24 反馈的形状不自然之一）。
    // 真实凝结高度随大尺度天气起伏 ⇒ 给底部 ±0.10 层厚（≈180 m）的低频起伏。
    // 频率用 1.0（q 空间），与风偏移的整数周期一致 ⇒ 无缝循环不被破坏。
    float wob = (cloudNoise3p(q + vec3(5.3, 11.7, 2.9), kCloudNoisePeriod) - 0.5) * 0.20;
    float prof = smoothstep(wob, wob + 0.055, h) * smoothstep(1.0, 0.52, h);
    d *= prof;

    // ⑤ 细节侵蚀（4× 频率、2× 风速 ⇒ 云的形状会相对"流动/翻滚"，而不是整体平移）
    //
    // 只对**主步进**做（detail_amp > 0）：自阴影的光程采样只关心"这条光程上有多少云"，
    // 不需要边缘的细碎形状 —— 而那 4 个 octave（32 次 hash）在这里是纯浪费（实测占
    // 密度成本约 1/4）。
    if (detail_amp > 0.0 && d > 0.001) {
        vec3 dq = q * 4.0 + vec3(7.3, 1.9, 4.1);
        dq.xz += uCloudWind * 8.0;
        float det = cloudFbm3pLod(dq, kCloudNoisePeriod * 4, 4, fp * 4.0);
        // 侵蚀在**云底更强**（真实积云的底面是被上升气流撕碎的），
        // 这一条专治"s7 云底像一块平板塑料"。
        float erode = mix(0.55, 0.22, smoothstep(0.0, 0.35, h));
        d = clamp(d - det * erode * (1.0 - d), 0.0, 1.0);
    }
    return d;
}

// Henyey-Greenstein 相函数（双瓣：前向 g=0.65 + 微弱背向）
float cloudPhase(float mu) {
    const float g1 = 0.65;
    const float g2 = -0.18;
    float denom1 = 1.0 + g1 * g1 - 2.0 * g1 * mu;
    float denom2 = 1.0 + g2 * g2 - 2.0 * g2 * mu;
    float p1 = (1.0 - g1 * g1) / (4.0 * 3.14159265 * pow(max(denom1, 1.0e-4), 1.5));
    float p2 = (1.0 - g2 * g2) / (4.0 * 3.14159265 * pow(max(denom2, 1.0e-4), 1.5));
    return (0.7 * p1 + 0.3 * p2) * 1.35;
}

// 一层云的积分（任意 base/thickness，便于叠高层卷云）
// pixel_angle_rad = 一个像素对应的**角尺寸**（弧度）。由调用方从 dFdx/dFdy(dir) 得出
// 并传进来：云密度的带限（LOD）要靠它算像素足迹，否则远处/掠射处的细节会走样成噪点。
vec4 marchCloudLayer(vec3 dir, vec3 camPos, float base_y, float thick,
                     float feature_m, float cov_mul, int steps, int kind,
                     float pixel_angle_rad,
                     vec3 bg, vec3 bg_up, vec3 lightCol, vec3 lightDir) {
    if (dir.y < 0.006) return vec4(0.0);
    float t0 = (base_y - camPos.y) / dir.y;
    float t1 = (base_y + thick - camPos.y) / dir.y;
    if (t1 <= 0.0) return vec4(0.0);
    t0 = max(t0, 0.0);

    const float kFarLimit = 32000.0;
    float far_fade = 1.0;
    if (t1 > kFarLimit) {
        far_fade = clamp((kFarLimit - t0) / max(t1 - t0, 1.0), 0.0, 1.0);
        t1 = min(t1, kFarLimit);
    }
    if (t1 <= t0) return vec4(0.0);

    float dt = (t1 - t0) / float(steps);
    // 像素足迹（米）——两个方向取大者：
    //   沿视线 = 步长 dt；横向 = t·角尺寸，且掠射时层交点距离对仰角极敏感
    //   （dt = t·dθ/dir.y）⇒ 必须按 1/dir.y 放大。漏了这一项，地平线一带就会
    //   用"近处的分辨率"去采噪声，整体糊成一层噪点（实测就是这个观感）。
    float inv_dir_y = 1.0 / max(dir.y, 0.02);
    float sigma = uCloudOpacity * 4.5 / max(thick, 1.0);
    float jitter = cloudHash21(floor(gl_FragCoord.xy * 0.5) * 1.37 + vec2(3.1, 7.7));

    float T = 1.0;
    vec3 L = vec3(0.0);
    for (int i = 0; i < steps; ++i) {
        float t = t0 + dt * (float(i) + jitter);
        vec3 pos = camPos + dir * t;
        float h = clamp((pos.y - base_y) / max(thick, 1.0), 0.0, 1.0);
        // 采样空间的**像素**足迹（单位：feature_m）—— 带限 fbm 靠它决定留到第几 octave。
        // 注意这里用「像素」而不是「步长」：步长决定的是**积分精度**（下面有细分），
        // 不是抗锯齿的带宽。把步长混进来会连同本来能分辨的细节一起抹掉。
        float fp = max(t * pixel_angle_rad * inv_dir_y, feature_m * 1.0e-4) / feature_m;

        float d = cloudDensity3(pos, h, base_y, thick, feature_m, fp, 5, 0.14, cov_mul, kind);
        if (d <= 0.002) continue;

        // 自阴影：3 个光程样本（近/中/远），覆盖整层的尺度
        float inv_thick = 1.0 / max(thick, 1.0);
        float od = 0.0;
        od += cloudDensity3(pos + lightDir * 140.0,
                            h + lightDir.y * 140.0 * inv_thick,
                            base_y, thick, feature_m, fp, 3, 0.0, cov_mul, kind) * 140.0;
        od += cloudDensity3(pos + lightDir * 420.0,
                            h + lightDir.y * 420.0 * inv_thick,
                            base_y, thick, feature_m, fp, 3, 0.0, cov_mul, kind) * 420.0;
        od += cloudDensity3(pos + lightDir * 1100.0,
                            h + lightDir.y * 1100.0 * inv_thick,
                            base_y, thick, feature_m, fp, 3, 0.0, cov_mul, kind) * 700.0;
        od *= sigma;

        float mu = dot(dir, lightDir);
        float mu_fwd = clamp(mu, 0.0, 1.0);

        // 环境天光（云的"半球照明"）：云底看到的多是地平线（暖），云顶看到的多是天顶
        // （冷）。夕阳时天顶是深蓝、地平线是橙 ⇒ 背光面偏紫、受光面偏橙。
        vec3 amb_sky = mix(bg, bg_up, mix(0.35, 0.80, h));
        vec3 amb = amb_sky * mix(0.07, 0.18, h);

        // 单次散射：很尖的 HG 瓣，被自己头顶的云衰减 exp(-od)。
        float single = cloudPhase(mu) * exp(-od);
        // 多次散射（Hillaire 2020 的退化版）：光子在水滴间来回弹射再出射 ⇒
        //  (a) 衰减慢得多（否则厚云全黑）；
        //  (b) 颜色被云内介质与**环境天光**混合 ⇒ 夕阳时暗部是"橙里透紫"，
        //      而不是纯橙。这一条是照片里冷暖对比的关键，用标量 sun 做不出来。
        float multi = 0.55 * exp(-od * 0.35) + 0.25 * exp(-od * 0.10);
        vec3 multi_col = mix(lightCol, amb_sky, 0.55);

        // 前向宽瓣：逆光时薄处被背后的太阳"透亮"，在太阳周围形成一圈暖光晕。
        // 只作用在**薄/边缘**（(1-d)），否则会把云心也刷暖、暗部丢掉冷色。
        float fwd = (0.45 * pow(mu_fwd, 3.0) + 0.12 * pow(mu_fwd, 1.2)) * (1.0 - d);

        // 云体自遮蔽（廉价 AO）：厚处压暗，亮部才留得住层次。
        float ao = mix(0.55, 1.12, 1.0 - d);
        vec3 sun_light = (lightCol * (single + fwd) + multi_col * multi) * ao;
        vec3 lum = amb + sun_light * 2.0;

        float ext = 1.0 - exp(-d * sigma * dt);
        L += T * lum * ext;
        T *= (1.0 - ext);
        if (T < 0.02) break;
    }

    float a = (1.0 - T) * far_fade;
    if (a < 1.0e-4) return vec4(0.0);
    vec3 col = L / max(1.0 - T, 1.0e-4);

    // ── 大气透视（aerial perspective）──
    //
    // 真实大气里，远处的云被视线路径上的空气散射"洗"过：对比度下降、颜色向该方向的
    // 天光色靠拢、并且整体变淡。缺了它，掠射方向上的云会是一堵**硬边、高对比的墙**
    //（Danis 反馈"地平线附近的云不自然"的另一半）。
    // 夕阳时这一项还额外送一个好处：远云被地平线那片橙光染色 ⇒ 火烧云的层次。
    float t_mid = 0.5 * (t0 + t1);
    float veil = 1.0 - exp(-t_mid / kAerialPerspectiveM);
    col = mix(col, bg, veil * kAerialMaxMix);
    a *= mix(1.0, 0.62, veil);

    return vec4(col * uCloudBrightness, clamp(a, 0.0, 1.0));
}

// 云板积分：scheme 6 = 单层积云；scheme 7 = 积云 + 高空卷云层
vec4 cloudSampleVolume(vec3 dir, vec3 camPos, float pixel_angle_rad,
                       vec3 bg, vec3 bg_up, vec3 lightCol, vec3 lightDir) {
    int kind = 0;
    if (uCloudScheme == 2) kind = 1;
    else if (uCloudScheme == 3) kind = 2;
    else if (uCloudScheme == 4) kind = 3;
    // 32 步：20 步时 dt 约 150–200 m，比噪声最细的 octave（150 m）还粗，
    // 细节根本没有被积出来、只能走样。带限之后多余的步数不会再走样（只会更准）。
    // 步数：默认 32（噪点下限，见 docs 第七轮）；交互期由 uCloudStepLimit 降档。
    const int kFullSteps = 32;
    int base_steps = uCloudStepLimit > 0 ? uCloudStepLimit : kFullSteps;
    vec4 base = marchCloudLayer(dir, camPos, uCloudHeight, uCloudThickness,
                                kCloudFeatureM, 1.0, base_steps, kind, pixel_angle_rad,
                                bg, bg_up, lightCol, lightDir);
    if (uCloudScheme != 7) return base;

    // 高层卷云（薄、覆盖大、特征更大）：让天空"有高度"，也让高低两层互相遮挡
    float cirrus_cov = 1.15;   // 卷云要薄但要看得见（0.55 时几乎不可见）
    // 卷云层固定 12 步；降档时按主层比例缩（下限 4，再少就只剩一条糊带）。
    int hi_steps = uCloudStepLimit > 0 ? max(uCloudStepLimit / 3, 4) : 12;
    vec4 hi = marchCloudLayer(dir, camPos, uCloudHeight * 3.4, 700.0,
                              kCloudFeatureM * 2.2, cirrus_cov, hi_steps, 2, pixel_angle_rad,
                              bg, bg_up, lightCol, lightDir);
    if (hi.a <= 1.0e-4) return base;
    if (base.a <= 1.0e-4) return hi;
    // hi over base
    vec3 col = hi.rgb * hi.a + base.rgb * base.a * (1.0 - hi.a);
    float a = hi.a + base.a * (1.0 - hi.a);
    return vec4(col / max(a, 1.0e-4), clamp(a, 0.0, 1.0));
}

// 云采样：返回 vec4(颜色, alpha)。
vec4 cloudSample(vec3 dir, vec3 camPos, vec3 bg, vec3 bg_up, vec3 lightCol, vec3 lightDir) {
    vec2 off = normalize(lightDir.xz + vec2(1.0e-4)) * 0.55;

    // 一个像素的角尺寸（弧度）= 方向在屏幕上的导数。
    // 必须在**统一控制流**里算（此处位于所有分支之前），且是体积方案 LOD 的唯一依据。
    float pixel_angle = max(length(dFdx(dir)), length(dFdy(dir)));

    if (uCloudScheme >= 1 && uCloudScheme <= 4) {
        // s1..s4 = **同一个体积链路**，只换密度场（Danis: "升级成完全体再对比"）
        return cloudSampleVolume(dir, camPos, pixel_angle, bg, bg_up, lightCol, lightDir);
    }
    if (uCloudScheme == 6 || uCloudScheme == 7) {
        return cloudSampleVolume(dir, camPos, pixel_angle, bg, bg_up, lightCol, lightDir);
    }

    if (uCloudScheme == 4) {
        // ── 多层视差云板（3 层，从高到低合成）──
        //
        // 试过真正的"沿视线步进积分"（2D 噪声 × 12~20 步）：从下往上看一个水平
        // 噪声面时，同一片云会被沿天顶方向拉成**竖直长条**，加抖动又变成颗粒，
        // 加斜切只是把条纹转到 45° —— 因为 2D 噪声沿视线方向根本不变化。
        // 真解需要 3D 噪声（llvmpipe 上 20 步 × 3 octave × 8 hash 太贵）。
        // 折中：多层平面。层间尺度/偏移不同 ⇒ 近地平线时有视差、有遮挡关系，
        // 且每层都是"真正的云块"，成本只有 3 次密度采样。
        vec3 col = vec3(0.0);
        float a = 0.0;
        for (int i = 2; i >= 0; --i) {
            float t = float(i);                       // 0 = 低（近），2 = 高（远）
            vec2 p = cloudPlane(dir) * uCloudScale * (0.85 + 0.55 * t) +
                     uCloudWind + vec2(t * 7.3, t * 3.1);
            float d = densityCumulus(p) * (1.0 - 0.22 * t);
            vec3 c = cloudBody(p, d, bg, lightCol, lightDir, 0);
            float ai = d * uCloudOpacity * 0.88;
            // over（后画的层（低、近）盖住先画的层）
            col = c * ai + col * (1.0 - ai);
            a = ai + a * (1.0 - ai);
        }
        return vec4(col, clamp(a * cloudHorizonFade(dir), 0.0, 1.0));
    }

    if (uCloudScheme == 5) {
        // ── 两层：卷云（薄、高）叠在积云上 ──
        vec2 p1 = cloudPlane(dir) * uCloudScale + uCloudWind;
        float d1 = densityCumulus(p1);
        vec3 c1 = cloudBody(p1, d1, bg, lightCol, lightDir, 0);
        float a1 = d1 * uCloudOpacity;
        vec2 p2 = cloudPlane(dir) * uCloudScale * 0.62 + uCloudWind * 0.7 + vec2(5.3, 9.1);
        float d2 = densityCirrus(p2);
        vec3 c2 = cloudBody(p2, d2, bg, lightCol, lightDir, 1) * 1.15;
        float a2 = d2 * uCloudOpacity * 0.8;
        vec3 col = c1 * a1 * (1.0 - a2) + c2 * a2;
        float a = (a1 + a2 * (1.0 - a1)) * cloudHorizonFade(dir);
        return vec4(col, clamp(a, 0.0, 1.0));
    }

    // ── 1/2/3：单层 ──
    vec2 p = cloudPlane(dir) * uCloudScale + uCloudWind;
    float d = densityMain(p);
    int layer = (uCloudScheme == 2) ? 2 : ((uCloudScheme == 3) ? 1 : 0);  // 不能 const：uniform 不是常量表达式
    vec3 col = cloudBody(p, d, bg, lightCol, lightDir, layer);
    // 银边：视线与光源夹角小 + 云薄（前向散射）
    float mu = max(dot(normalize(dir), normalize(lightDir)), 0.0);
    col += lightCol * (pow(mu, 14.0) * (1.0 - d) * 1.8);
    return vec4(col, d * uCloudOpacity * cloudHorizonFade(dir));
}
)glsl";

// ─────────────────────────────────────────────────────────────────────────────
// CPU 同源实现（只覆盖「太阳方向上的云量」这个用途，不做完整着色）
// ─────────────────────────────────────────────────────────────────────────────

inline uint32_t CloudHash(uint32_t x) {
    x ^= x >> 16;
    x *= 2146121005u;
    x ^= x >> 15;
    x *= 2221713035u;
    x ^= x >> 16;
    return x;
}

inline float CloudHash21(float x, float y) {
    const int ix = static_cast<int>(std::floor(x));
    const int iy = static_cast<int>(std::floor(y));
    const uint32_t h = CloudHash(static_cast<uint32_t>(ix) * 1973u +
                                 static_cast<uint32_t>(iy) * 9277u + 26699u);
    return static_cast<float>(h) * (1.0f / 4294967296.0f);
}

inline float CloudNoise(float x, float y) {
    const float fx = std::floor(x);
    const float fy = std::floor(y);
    const float ux = x - fx;
    const float uy = y - fy;
    const float wx = ux * ux * (3.0f - 2.0f * ux);
    const float wy = uy * uy * (3.0f - 2.0f * uy);
    const float a = CloudHash21(fx, fy);
    const float b = CloudHash21(fx + 1.0f, fy);
    const float c = CloudHash21(fx, fy + 1.0f);
    const float d = CloudHash21(fx + 1.0f, fy + 1.0f);
    const float m0 = a + (b - a) * wx;
    const float m1 = c + (d - c) * wx;
    return m0 + (m1 - m0) * wy;
}

// 与 GLSL cloudFbm 同参数（旋转 53°、每层 ×2.02）
inline float CloudFbm(float x, float y, int octaves) {
    const float kCos = 0.601815f;
    const float kSin = 0.798635f;
    float sum = 0.0f;
    float amp = 0.5f;
    float norm = 0.0f;
    for (int i = 0; i < octaves; ++i) {
        sum += amp * CloudNoise(x, y);
        norm += amp;
        const float nx = (kCos * x - kSin * y) * 2.02f;
        const float ny = (kSin * x + kCos * y) * 2.02f;
        x = nx;
        y = ny;
        amp *= 0.5f;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}

inline float CloudWorley(float x, float y) {
    const float fx = std::floor(x);
    const float fy = std::floor(y);
    const float px = x - fx;
    const float py = y - fy;
    float best = 8.0f;
    for (int gy = -1; gy <= 1; ++gy) {
        for (int gx = -1; gx <= 1; ++gx) {
            const float o1 = CloudHash21(fx + static_cast<float>(gx) + 11.0f,
                                         fy + static_cast<float>(gy) + 5.0f);
            const float o2 = CloudHash21(fx + static_cast<float>(gx) + 37.0f,
                                         fy + static_cast<float>(gy) + 91.0f);
            const float rx = static_cast<float>(gx) + o1 - px;
            const float ry = static_cast<float>(gy) + o2 - py;
            best = std::min(best, rx * rx + ry * ry);
        }
    }
    return std::sqrt(best);
}

// 与 GLSL cloudCover 同参数
inline float CloudCover(float n, const CloudCommand& c) {
    const float edge = 1.0f - c.coverage;
    const float soft = std::max(c.softness, 0.001f);
    if (n <= edge) return 0.0f;
    if (n >= edge + soft) return 1.0f;
    const float t = (n - edge) / soft;
    return t * t * (3.0f - 2.0f * t);
}

// 某个**方向**上的云量（0..1）。用于「太阳/月亮被云遮了多少」这类 CPU 侧推导。
// 覆盖方案 1/2/3/5 的 2D 密度；方案 4 用同一份基础层（云板在地平线附近近似）。
inline float CoverageAtDirection(const CloudCommand& c, const Vec3f& dir) {
    if (!c.enabled) return 0.0f;
    const Vec3f u = dir.Unit();
    const float denom = 1.0f + std::max(u.y(), -0.9f);
    const float px = u.x() / denom * c.scale + c.wind_offset.x();
    const float py = u.z() / denom * c.scale + c.wind_offset.y();
    const int scheme = static_cast<int>(c.scheme);
    if (scheme == 6) {
        // 3D 体积方案：用天气图（大尺度云量）近似"太阳方向被遮了多少"
        const float w = CloudFbm(px * (1.0f / 9000.0f) * 1.7f + 11.0f,
                                 py * (1.0f / 9000.0f) * 1.7f + 5.0f, 2);
        const float cov = std::min(std::max(c.coverage * (0.55f + 0.90f * w), 0.0f), 0.98f);
        return cov;
    }
    if (scheme == 2) {
        const float wx = CloudFbm(px * 0.18f + 11.3f, py * 0.18f + 4.1f, 2) * 2.4f - 1.2f;
        const float wy = CloudFbm(px * 0.18f + 41.7f, py * 0.18f + 27.9f, 2) * 2.4f - 1.2f;
        const float w = 1.0f - CloudWorley((px + wx) * 0.55f, (py + wy) * 0.55f);
        const float det = CloudFbm(px * 1.8f + 3.7f, py * 1.8f + 1.9f, 3);
        const float patch = CloudFbm(px * 0.22f + 71.3f, py * 0.22f + 13.7f, 2);
        return CloudCover(w * 0.62f + det * 0.18f + patch * 0.20f, c);
    }
    if (scheme == 3) {
        const float a = CloudFbm(px * 0.42f, py * 1.60f, 3);
        const float b = CloudFbm(px * 0.42f * 3.1f + 17.3f, py * 1.60f * 3.1f + 9.1f, 2);
        return CloudCover(a * 0.74f + b * 0.26f, c) * 0.72f;
    }
    const float patch = CloudFbm(px * 0.22f + 19.1f, py * 0.22f + 3.3f, 2);
    const float n = CloudFbm(px, py, 4);
    return CloudCover(n + (patch - n) * 0.28f, c);
}

}  // namespace cloud
}  // namespace jpov

#endif  // JPOV_SRC_SKYDOME_CLOUD_SHADER_H_
