// JPOV FireParticleEmitter —— CPU 粒子模拟（火焰/燃烧用），产出 FireCommand 快照
//
// 设计定位（见 docs/jpov_effect_pass_design.md）：
//   - **有状态的模拟层**（粒子池 + 生命周期 + 轨迹积分）住在这一层；
//   - 每帧模拟完，把「每颗粒子此刻的样子」写成一条 FireCommand（无状态快照）；
//   - 渲染层只负责忠实绘制（FireRenderer）。
//
// 为什么粒子能解决「一眼假」：火焰由**大量各自上升的火苗**组成，没有单个
// 闭合轮廓可被看穿；每个粒子有独立的初速/相位/寿命 → 自然、不齐。
//
// 本文件 **GL-free 纯 CPU**（只产出命令），可单测、可复现（确定性哈希 + 固定 dt）。

#ifndef JPOV_EFFECT_PARTICLE_FIRE_FIRE_PARTICLES_H_
#define JPOV_EFFECT_PARTICLE_FIRE_FIRE_PARTICLES_H_

#include <cmath>
#include <cstdint>
#include <vector>

#include "tools/jpov/interface/render_command.h"

namespace jpov {

// 一颗粒子（内部状态）。
struct FireParticleState {
    Vec3f pos;          // 世界位置
    Vec3f vel;          // 速度（米/秒）
    float age = 0.0f;   // 已活时间（秒）
    float life = 1.0f;  // 总寿命（秒）
    float half_w = 0.1f;   // 出生命题的子面片半宽（米）
    float grow = 1.6f;     // 尺寸随生命的倍率（末尾尺寸 = half_w * grow）
    float phase = 0.0f;    // 动画相位（秒，喂 FireCommand::time_offset）
    float speed = 1.0f;    // 动画速度倍率（各自噪声滚动快慢）
    bool alive = false;
};

// 火焰粒子发射器：固定池 + 连续发射 + 上升/摆动 + 寿命回收。
//
// 用法（每帧）：
//   emitter.SetSource(0.9f);              // 每秒发射率（按需要）
//   emitter.SpawnAt(base, spread);        // 从某点/某圈发射
//   emitter.Update(dt);                   // 推进模拟
//   emitter.Append(cmds, core, outer, ...);// 产出 FireCommand
class FireParticleEmitter {
public:
    // 最大粒子数（池上限）。火焰用 300~1200 比较合适（llvmpipe 填充率约束）。
    explicit FireParticleEmitter(size_t max_particles = 600) {
        pool_.resize(max_particles);
        next_ = 0;
    }

    // 设定每秒发射率。
    void SetRate(float rate_per_sec) { rate_ = rate_per_sec; }
    float rate() const { return rate_; }

    // 发射：从 src 附近（半径 spread 的球内）喷出一颗，带初速。
    // up_speed：初始向上速度；jit：横向抖动速度。
    // seed：确定性来源（调用方自增）。
    void SpawnOne(const Vec3f& src, float spread, float up_speed, float jit,
                  uint32_t seed) {
        FireParticleState* p = Acquire();
        if (p == nullptr) {
            return;   // 池满：本次不发射（不无限增长）
        }
        const float r1 = Hash2Float(seed, 1u);
        const float r2 = Hash2Float(seed, 2u);
        const float r3 = Hash2Float(seed, 3u);
        const float r4 = Hash2Float(seed, 4u);
        // 球内随机偏移（立方根分布，保证均匀）。
        const float theta = 6.2831853f * r1;
        const float u = 2.0f * r2 - 1.0f;
        const float s = std::sqrt(std::max(0.0f, 1.0f - u * u));
        const float rad = spread * std::cbrt(r3);
        p->pos = src + Vec3f(rad * s * std::cos(theta), rad * u,
                             rad * s * std::sin(theta));
        p->vel = Vec3f(jit * (2.0f * r1 - 1.0f), up_speed,
                       jit * (2.0f * r4 - 1.0f));
        p->age = 0.0f;
        p->life = 0.45f + 0.75f * r2;                 // 0.45~1.2 s
        p->half_w = 0.05f + 0.07f * r3;               // 5~12 cm 半宽
        p->grow = 1.4f + 0.9f * r1;                   // 变大倍率
        p->phase = 12.0f * r4;                        // 相位错开
        p->speed = 0.9f + 0.9f * r1;                  // 各自噪声速度
        p->alive = true;
        ++emitted_;
    }

    // 按发射率累积并发射 count 颗（dt 内应发数 = rate * dt）。
    // 调用方给出「火源点」列表（如柱底一圈），本函数轮流从各源发射。
    void EmitAccumulated(float dt, const std::vector<Vec3f>& sources,
                         float spread, float up_speed, float jit) {
        acc_ += rate_ * dt;
        while (acc_ >= 1.0f) {
            acc_ -= 1.0f;
            if (sources.empty()) {
                break;
            }
            const uint32_t seed = emit_seed_++;
            const Vec3f& src = sources[seed % sources.size()];
            SpawnOne(src, spread, up_speed, jit, seed);
        }
    }

    // 推进模拟：浮力 + 摆动 + 老化（各自的相位/速度 → 各自飘）。
    // buoyancy：向上加速度；sway：横向摆动幅度（随高度增大）；drag：阻尼。
    void Update(float dt, float buoyancy = 2.6f, float sway = 1.1f,
                float drag = 1.4f) {
        for (FireParticleState& p : pool_) {
            if (!p.alive) {
                continue;
            }
            p.age += dt;
            if (p.age >= p.life) {
                p.alive = false;
                continue;
            }
            const float t = p.age / p.life;           // 0..1
            // 浮力（越往上越小）+ 阻尼
            p.vel = Vec3f(p.vel.x(), p.vel.y() + buoyancy * (1.0f - t) * dt,
                          p.vel.z());
            p.vel = Vec3f(p.vel.x() / (1.0f + drag * dt),
                          p.vel.y() / (1.0f + drag * 0.3f * dt),
                          p.vel.z() / (1.0f + drag * dt));
            // 横向摆动：随生命/高度增大（“火苗摆动大”）
            const float w = sway * t * dt;
            p.pos = Vec3f(p.pos.x() + p.vel.x() * dt + w * std::sin(p.phase + p.age * 5.0f),
                          p.pos.y() + p.vel.y() * dt,
                          p.pos.z() + p.vel.z() * dt +
                              w * std::cos(p.phase * 1.3f + p.age * 4.0f));
        }
    }

    // 产出命令：每颗粒子 = 一条 FireCommand（逐条命令，和 PointLight 一桌）。
    //   color_hot / color_cold：生命两端颜色（中间插值）；
    //   intensity：整体发光强度；blend：混合模式。
    void Append(RenderCommandList* cmds,
                const Color& color_hot, const Color& color_cold,
                float intensity, ParticleBlend blend,
                float noise_scale = 3.4f) const {
        for (const FireParticleState& p : pool_) {
            if (!p.alive) {
                continue;
            }
            const float t = p.age / p.life;
            // 尺寸：先胀后缩（末尾略缩）。
            const float size_mul = 1.0f + (p.grow - 1.0f) * std::sin(t * 3.14159f);
            const float half_w = p.half_w * size_mul;
            // 颜色随生命：热 → 冷。
            const Color c = LerpColor(color_hot, color_cold, t);
            // 亮度随生命衰减（末尾熄灭）。
            const float fade = (1.0f - t) * (1.0f - t);
            cmds->DrawFire(/*base*/ p.pos,
                           /*radius*/ half_w,
                           /*height*/ half_w * 2.4f,
                           /*color_core*/ c,
                           /*color_outer*/ c,
                           /*intensity*/ intensity * (0.35f + 0.65f * fade),
                           /*speed*/ p.speed,
                           /*noise_scale*/ noise_scale,
                           /*blend*/ blend,
                           /*time_offset*/ p.phase);
        }
    }

    size_t AliveCount() const {
        size_t n = 0;
        for (const FireParticleState& p : pool_) {
            if (p.alive) {
                ++n;
            }
        }
        return n;
    }
    size_t Capacity() const { return pool_.size(); }
    size_t Emitted() const { return emitted_; }

private:
    FireParticleState* Acquire() {
        // 环形扫描找空位（O(pool)，池小可接受；找不到 = 池满）。
        for (size_t i = 0; i < pool_.size(); ++i) {
            const size_t k = (next_ + i) % pool_.size();
            if (!pool_[k].alive) {
                next_ = (k + 1) % pool_.size();
                return &pool_[k];
            }
        }
        return nullptr;
    }

    static uint32_t HashMix(uint32_t x) {
        x ^= x >> 16;
        x *= 0x7feb352dU;
        x ^= x >> 15;
        x *= 0x846ca68bU;
        x ^= x >> 16;
        return x;
    }
    static float Hash2Float(uint32_t a, uint32_t b) {
        const uint32_t h = HashMix(a * 2654435761u + b * 40503u);
        return static_cast<float>(h >> 8) / 16777216.0f;
    }
    static Color LerpColor(const Color& a, const Color& b, float t) {
        const float tc = std::max(0.0f, std::min(1.0f, t));
        return Color{a.r + (b.r - a.r) * tc, a.g + (b.g - a.g) * tc,
                     a.b + (b.b - a.b) * tc, a.a + (b.a - a.a) * tc};
    }

    std::vector<FireParticleState> pool_;
    size_t next_ = 0;
    float rate_ = 60.0f;      // 每秒发射数
    float acc_ = 0.0f;        // 发射累积器
    uint32_t emit_seed_ = 1;  // 发射种子（确定性）
    size_t emitted_ = 0;
};

}  // namespace jpov

#endif  // JPOV_EFFECT_PARTICLE_FIRE_FIRE_PARTICLES_H_
