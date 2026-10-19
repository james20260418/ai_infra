// JPOV 广告牌粒子火 — 模拟内核（GL-free、确定性、可单测）
//
// 本文件是「主角火」广告牌粒子方案的**模拟大脑**：把一份发射器配置（EmitterConfig）
// 随时间推进成一批 ParticleInstance（位置/速度/尺寸/朝向角/颜色/flipbook 帧号）。
// 渲染只是把这些实例当广告牌面片画出来 —— 模拟与渲染彻底解耦（见 SOUL「Separate
// What from How」）。
//
// 设计：tools/jpov/docs/jpov_particle_fire_design.md。
// 特性**大多以开关暴露**（bool/enum 字段）：某个特性实现有 bug 时可单独关掉，不影响
// 其余特性验证（2026-10-14 Danis 定）。
//
// 确定性：所有随机取自带种子的 Rng；同 (config,seed,dt 序列) ⇒ 逐步逐位可复现。
//
// 坐标系：世界空间、y 向上（与 JPOV 一致）。

#ifndef JPOV_SRC_PARTICLES_PARTICLE_SYSTEM_H_
#define JPOV_SRC_PARTICLES_PARTICLE_SYSTEM_H_

#include <cstdint>
#include <vector>

#include "geom/common/vec.h"
#include "tools/jpov/interface/particle_types.h"
#include "tools/jpov/interface/pbr_material.h"

namespace jpov {
namespace particles {

using Vec2f = geom::Vec2<float>;
using Vec3f = geom::Vec3<float>;

// ==================== 确定性随机 ====================

// splitmix64：小而快的确定性 PRNG。不追求密码学强度，只要可复现、分布均匀。
class Rng {
public:
    explicit Rng(uint64_t seed) : state_(seed + 0x9E3779B97F4A7C15ull) {}

    uint64_t NextU64();
    // [0,1) 均匀。
    float Next01();
    // [lo,hi) 均匀（hi>lo 由调用方保证；lo==hi 时返回 lo）。
    float Range(float lo, float hi);

private:
    uint64_t state_;
};

// ==================== 曲线 / 渐变 ====================

// 生命期区间（出生时按此随机采样一次）。
struct RangeF {
    float min = 0.0f;
    float max = 0.0f;
    // Pre-condition: max >= min。
    float Sample(Rng& rng) const;
};

// 单值曲线：t∈[0,1] → v。keys 按 t 升序；线性插值；端点外取端点值。
// keys 为空 ⇒ Eval 返回 0（由调用方避免空曲线）。
struct Curve {
    struct Key {
        float t = 0.0f;
        float v = 0.0f;
    };
    std::vector<Key> keys;
    float Eval(float t) const;
};

// 颜色渐变：t∈[0,1] → Color（RGBA 线性插值）。
struct Gradient {
    struct Key {
        float t = 0.0f;
        Color c;
    };
    std::vector<Key> keys;
    Color Eval(float t) const;
};

// ==================== 配置枚举 ====================

// 发射形状。
enum class ShapeKind : uint8_t {
    kPoint = 0,   // 单点
    kCircle = 1,  // 水平圆盘（XZ 平面），半径 radius
    kCone = 2,    // 锥体：底圆半径 radius，张角 2*cone_half_angle_deg（绕 emit_dir）
    kBox = 3,     // 轴对齐盒，半尺寸 box_half
};

// flipbook 帧推进方式（对齐 Unity Texture Sheet Animation 的 Time Mode）。
enum class SheetTimeMode : uint8_t {
    kLifetime = 0,  // 按寿命用 Curve 采样帧（frame_curve）
    kFps = 1,       // 按固定帧率循环
    kSpeed = 2,     // 按粒子速度映射到 [0,total_frames)
    kRandomStatic = 3,  // 每粒子固定一帧
    kPairMorph = 4,     // 每粒子随机选一个「块」，寿命内从块首走到块尾（pair_len 帧/块）
};

// ==================== 发射器配置 ====================
// 广告牌朝向（AlignMode）/ 混合（BlendMode）/ 排序（SortMode）/ 输出实例
// （ParticleInstance）定义在 interface/particle_types.h（接口层，与渲染命令共用）。

// 一个发射器 = 一层火（核心焰 / 主火舌 / 烟 / 火星 …）。
// 所有字段默认值即「一根向上冒的加法火柱」的合理起点。
struct EmitterConfig {
    // ---- 发射 ----
    Vec3f origin = {0.0f, 0.0f, 0.0f};  // 发射原点（世界）
    float rate = 40.0f;                 // 每秒发射粒子数（>= 0）
    int burst = 0;                      // t=0 一次性额外发射数（>0 才生效）
    ShapeKind shape = ShapeKind::kPoint;
    float radius = 0.1f;                // kCircle/kCone 底圆半径（>= 0）
    float cone_half_angle_deg = 20.0f;  // kCone 半张角（度，[0,90]）
    Vec3f box_half = {0.1f, 0.1f, 0.1f};  // kBox 半尺寸（各 >= 0）
    Vec3f emit_dir = {0.0f, 1.0f, 0.0f};   // 基准发射方向（会被归一化）

    // ---- 出生状态 ----
    RangeF lifetime = {1.0f, 1.4f};      // 秒
    RangeF speed = {0.6f, 1.0f};         // m/s
    RangeF size_start = {0.35f, 0.5f};   // 出生尺寸（世界米，方形基准，乘 size_curve）
    RangeF rotation = {0.0f, 6.2831853f};  // 出生朝向角（弧度）
    // 自旋角速度（弧度/秒，可负）。参考 Astralax 火的做法：粒子一边烧一边自旋，
    // 且自旋量随寿命衰减（末端不乱转）→ 由 rotation_speed_decay 控制。
    RangeF rotation_speed = {0.0f, 0.0f};
    float rotation_speed_decay = 0.0f;   // 0=匀速；1=到寿命末端降到 0
    Color color_start = {1.0f, 0.65f, 0.2f, 1.0f};

    // ---- 受力 ----
    Vec3f gravity = {0.0f, 1.4f, 0.0f};  // 加速度（火：+Y 浮力）
    float drag = 0.6f;                   // 阻尼（1/s，>=0；速度乘 (1-drag*dt)）
    bool noise_enabled = false;          // 湍流开关
    float noise_amp = 0.0f;              // 湍流加速度幅度（m/s²）
    float noise_freq = 1.5f;             // 湍流空间频率

    // ---- 生命期演化（开关）----
    bool use_color_over_life = true;     // 关：用 color_start + 线性淡出 alpha
    Gradient color_over_life;            // 温度梯度（默认见 MakeDefaultFireGradient）
    bool use_size_over_life = true;      // 关：尺寸恒定 = 出生采样值
    Curve size_curve;                    // 尺寸倍率曲线（默认见 MakeDefaultFireSizeCurve）

    // ---- flipbook ----
    bool flipbook = true;                // 关：不裁 UV（整图当一帧）
    int tiles_x = 8;                     // 网格列（>=1）
    int tiles_y = 8;                     // 网格行（>=1）
    SheetTimeMode sheet_time_mode = SheetTimeMode::kFps;
    float sheet_fps = 18.0f;             // kFps 用（帧/秒）
    float sheet_cycles = 1.0f;           // 寿命内循环遍数（kLifetime 用）
    bool start_frame_random = true;      // 每粒子随机起始相位（kFps/kSpeed 用）
    int pair_len = 2;                    // kPairMorph：每个块占多少帧（>=2）
    Curve frame_curve;                   // kLifetime 用：t→[0,1) 帧比例

    // ---- 渲染 ----
    uint32_t texture_id = 0;             // flipbook 纹理（TextureManager id；0=纯色）
    AlignMode align = AlignMode::kVertical;
    bool stretched = false;              // 沿速度拉伸（kVelocity 时才有意义）
    float stretch_scale = 1.0f;          // 拉伸系数（每 m/s）
    BlendMode blend = BlendMode::kAdditive;
    SortMode sort = SortMode::kNone;
    float soft_distance = 0.0f;          // 软粒子淡出距离（>0 才生效；MVP 未接）

    // Pre-condition: rate>=0；radius>=0；size_start.max>0；tiles_x/y>=1。
    void Validate() const;
};

// 默认「火」用的温度色带：白热 → 黄 → 橙 → 红 → 暗/透明。
Gradient MakeDefaultFireGradient();
// 默认尺寸曲线：小 → 大 → 末段收缩。
Curve MakeDefaultFireSizeCurve();
// 默认帧曲线：线性 0→1。
Curve MakeDefaultLinearFrameCurve();

// ==================== 发射器（有状态） ====================

// 单个发射器：持有存活粒子列表，Update() 逐帧推进并**追加**输出实例。
class ParticleEmitter {
public:
    ParticleEmitter() = default;
    explicit ParticleEmitter(EmitterConfig cfg);

    // 重置：清空粒子、按种子重启发射时钟（可选做一次 burst）。确定性起点。
    void Reset(uint32_t seed);

    // 替换配置（不重置已有粒子）。改形状/尺寸等只影响后续出生的粒子。
    void SetConfig(const EmitterConfig& cfg);

    const EmitterConfig& config() const { return cfg_; }

    // 推进 dt 秒：先在源处补发本帧应生成的粒子，再对全体粒子积分，
    // 回收超寿命者，并把存活的 ParticleInstance **追加**到 out（不清空 out）。
    // deaths 非空时，把本帧**死亡**粒子的位置追加进去（子发射器 / spawn-on-death 用）。
    // Pre-condition: dt >= 0。
    void Update(float dt, std::vector<ParticleInstance>* out /*output*/,
                std::vector<Vec3f>* deaths /*output，可空*/ = nullptr);

    // 在指定位置**注入**一个粒子（子发射器用）：位置给定，速度取本发射器的 emit_dir×speed
    //（+少量水平抖动），其余出生状态（寿命/尺寸/朝向/flipbook 相位/种子）按本配置采样。
    // 用于「火焰粒子死亡 → 在火苗顶端生成烟」这类 spawn-on-death 子发射器。
    // 达到存活上限时静默丢弃。
    void SpawnAt(const Vec3f& position);

    int live_count() const { return static_cast<int>(particles_.size()); }

private:
    struct Particle {
        Vec3f position;
        Vec3f velocity;
        float age = 0.0f;
        float lifetime = 1.0f;
        float rotation0 = 0.0f;
        float rotation = 0.0f;   // 当前朝向（含自旋累积）
        float spin0 = 0.0f;      // 出生自旋角速度
        float size0 = 0.0f;      // 出生尺寸采样
        float frame_phase = 0.0f;  // flipbook 起始相位 [0,1)
        uint32_t seed = 0;       // 逐粒子种子（噪声相位）
    };

    // 生成一个粒子（在发射源处，含发射形状/初速）。
    Particle Spawn();
    // 生成一个粒子（位置/初速由调用方给出；子发射器 SpawnAt 用）。
    Particle MakeParticle(const Vec3f& position, const Vec3f& velocity);
    // 采样发射位置/方向。
    void SampleEmission(Rng& rng, Vec3f* position /*output*/, Vec3f* direction /*output*/);
    // 解算某粒子当前帧的 flipbook 帧号。
    float ResolveFrame(const Particle& p) const;

    EmitterConfig cfg_;
    Rng rng_{0};
    std::vector<Particle> particles_;
    float spawn_accumulator_ = 0.0f;  // rate 余量（保证平均发射率精确）
    bool primed_ = false;             // 是否已做过首次 burst
};

}  // namespace particles
}  // namespace jpov

#endif  // JPOV_SRC_PARTICLES_PARTICLE_SYSTEM_H_
