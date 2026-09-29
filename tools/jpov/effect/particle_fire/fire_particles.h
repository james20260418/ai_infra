// JPOV FireParticleEmitter —— CPU 粒子模拟（火焰/燃烧用），产出命令快照
//
// 设计定位（见 docs/jpov_effect_pass_design.md）：
//   - **有状态的模拟层**（粒子池 + 生命周期 + 轨迹积分 + 小尺度湍流）住在这一层；
//   - 每帧模拟完，把「每颗粒子此刻的样子」写成**无状态命令快照**；
//   - 渲染层只负责忠实绘制。
//
// 为什么粒子能解决「一眼假」：火焰由**大量各自上升的火苗**组成，没有单个
// 闭合轮廓可被看穿；每颗粒子有独立的初速/相位/寿命 → 自然、不齐。
//
// 三种「动力学风格」（对应 render_command.h 的 ParticleStyle）：
//   kSoftPuff：慢、面积大、圆软团（“烟变火”）。
//   kTongue  ：快、细长上尖、强摆动（火舌）。
//   kVortex  ：**物理涡流**——速度叠加 curl-noise（无散度旋涡场）→ 被卷起/撕碎。
//
// 本文件 **GL-free 纯 CPU**（只产出命令），可单测、可复现（确定性哈希 + 固定 dt）。

#ifndef JPOV_EFFECT_PARTICLE_FIRE_FIRE_PARTICLES_H_
#define JPOV_EFFECT_PARTICLE_FIRE_FIRE_PARTICLES_H_

#include <algorithm>
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
    float size = 0.1f;  // 出生尺寸（米）
    float grow = 1.5f;  // 尺寸随生命的最大倍率
    float phase = 0.0f; // 动画相位（秒）
    float speed = 1.0f; // 动画速度倍率
    bool alive = false;
};

class FireParticleEmitter {
public:
    // 动力学风格（与 ParticleStyle 同名同序）。
    enum class Mode { kSoftPuff = 0, kTongue = 1, kVortex = 2 };

    explicit FireParticleEmitter(size_t max_particles = 1200) {
        pool_.resize(max_particles);
    }

    void SetMode(Mode m) { mode_ = m; }
    Mode mode() const { return mode_; }
    void SetRate(float rate_per_sec) { rate_ = rate_per_sec; }
    float rate() const { return rate_; }

    // 清空所有粒子（切换风格时用，避免两种形状混在一起）。
    void Clear() {
        for (FireParticleState& p : pool_) {
            p.alive = false;
        }
        acc_ = 0.0f;
    }

    // 从 src 附近（球内，半径 spread）喷一颗，带初速与各风格参数。
    void SpawnOne(const Vec3f& src, float spread, uint32_t seed) {
        FireParticleState* p = Acquire();
        if (p == nullptr) {
            return;   // 池满：不发射（不无限增长）
        }
        const float r1 = Hash2Float(seed, 1u);
        const float r2 = Hash2Float(seed, 2u);
        const float r3 = Hash2Float(seed, 3u);
        const float r4 = Hash2Float(seed, 4u);
        const float theta = 6.2831853f * r1;
        const float u = 2.0f * r2 - 1.0f;
        const float s = std::sqrt(std::max(0.0f, 1.0f - u * u));
        const float rad = spread * std::cbrt(r3);
        p->pos = src + Vec3f(rad * s * std::cos(theta), rad * u,
                             rad * s * std::sin(theta));
        p->age = 0.0f;
        p->phase = 20.0f * r4;
        p->speed = 0.8f + 1.0f * r1;

        // ── 各风格初值 ──
        if (mode_ == Mode::kSoftPuff) {
            p->vel = Vec3f(0.16f * (2.0f * r1 - 1.0f), 0.45f + 0.35f * r2,
                           0.16f * (2.0f * r4 - 1.0f));
            p->life = 0.9f + 1.2f * r2;
            p->size = 0.10f + 0.14f * r3;
            p->grow = 2.0f + 1.2f * r1;
        } else if (mode_ == Mode::kTongue) {
            p->vel = Vec3f(0.30f * (2.0f * r1 - 1.0f), 1.15f + 0.55f * r2,
                           0.30f * (2.0f * r4 - 1.0f));
            p->life = 0.32f + 0.50f * r2;
            p->size = 0.055f + 0.06f * r3;
            p->grow = 1.05f + 0.5f * r1;
        } else {   // kVortex
            p->vel = Vec3f(0.22f * (2.0f * r1 - 1.0f), 0.55f + 0.45f * r2,
                           0.22f * (2.0f * r4 - 1.0f));
            p->life = 0.8f + 1.0f * r2;
            p->size = 0.10f + 0.12f * r3;
            p->grow = 1.5f + 0.9f * r1;
        }
        p->alive = true;
        ++emitted_;
    }

    // 按发射率累积并发射（dt 内应发 rate*dt 颗）；来源轮流取。
    void EmitAccumulated(float dt, const std::vector<Vec3f>& sources, float spread) {
        acc_ += rate_ * dt;
        while (acc_ >= 1.0f) {
            acc_ -= 1.0f;
            if (sources.empty()) {
                break;
            }
            const uint32_t seed = emit_seed_++;
            SpawnOne(sources[seed % sources.size()], spread, seed);
        }
    }

    // 推进模拟：浮力 + 各风格纵向/横向行为 + 涡流模式的 curl-noise 力 + 老化。
    void Update(float dt, float t_seconds) {
        for (FireParticleState& p : pool_) {
            if (!p.alive) {
                continue;
            }
            p.age += dt;
            if (p.age >= p.life) {
                p.alive = false;
                continue;
            }
            const float t = p.age / p.life;

            if (mode_ == Mode::kSoftPuff) {
                p.vel = Vec3f(p.vel.x(), p.vel.y() + 1.9f * (1.0f - t) * dt, p.vel.z());
                p.vel = Damp(p.vel, 1.0f, dt);
                const float w = 0.55f * t * dt;
                p.pos = Vec3f(p.pos.x() + p.vel.x() * dt + w * std::sin(p.phase + p.age * 2.0f),
                              p.pos.y() + p.vel.y() * dt,
                              p.pos.z() + p.vel.z() * dt + w * std::cos(p.phase * 1.3f + p.age * 1.7f));
            } else if (mode_ == Mode::kTongue) {
                p.vel = Vec3f(p.vel.x(), p.vel.y() + 4.2f * (1.0f - t) * dt, p.vel.z());
                p.vel = Damp(p.vel, 1.7f, dt);
                // 强摆动（“左右摆动明显”）：随生命增大。
                const float w = 2.2f * t * dt;
                p.pos = Vec3f(p.pos.x() + p.vel.x() * dt + w * std::sin(p.phase + p.age * 5.5f),
                              p.pos.y() + p.vel.y() * dt,
                              p.pos.z() + p.vel.z() * dt + w * std::cos(p.phase * 1.7f + p.age * 4.5f));
            } else {   // kVortex：物理涡流（curl-noise 无散度旋涡场）
                const Vec3f c = CurlNoise(p.pos * 0.9f, t_seconds + p.phase);
                p.vel = Vec3f(p.vel.x(), p.vel.y() + 2.3f * (1.0f - t) * dt, p.vel.z());
                p.vel = Vec3f(p.vel.x() + c.x() * 1.5f * dt,
                              p.vel.y() + c.y() * 0.9f * dt,
                              p.vel.z() + c.z() * 1.5f * dt);
                p.vel = Damp(p.vel, 0.9f, dt);
                p.pos = Vec3f(p.pos.x() + p.vel.x() * dt,
                              p.pos.y() + p.vel.y() * dt,
                              p.pos.z() + p.vel.z() * dt);
            }
        }
    }

    // ── 产出：粒子快照命令（ParticleCommand）──
    // heat 随生命从 1 降到 0（热→冷 → 颜色由 shader 映射）；
    // alpha 淡入淡出；size 先胀后缩；aspect 由风格决定（火舌细长）。
    void AppendParticles(RenderCommandList* cmds, const Color& color_hot,
                         const Color& color_cold, float intensity,
                         ParticleBlend blend) const {
        const ParticleStyle style = ToStyle(mode_);
        const float aspect = (mode_ == Mode::kTongue) ? 2.6f : 1.0f;
        for (const FireParticleState& p : pool_) {
            if (!p.alive) {
                continue;
            }
            const float t = p.age / p.life;
            const float size_mul = 1.0f + (p.grow - 1.0f) * std::sin(t * 3.14159f);
            const float size = p.size * size_mul;
            const float heat = std::max(0.0f, 1.0f - 0.95f * t);
            const float fade = std::sin(std::min(1.0f, t * 1.15f) * 3.14159f);
            const float alpha = std::max(0.0f, fade) * (0.55f + 0.45f * heat);
            // 颜色：两端色原样交给 shader，由 heat 在片元里插值（不在此预混）。
            cmds->DrawParticle(/*position*/ p.pos, /*size*/ size,
                               /*aspect*/ aspect, /*heat*/ heat,
                               /*alpha*/ alpha * intensity,
                               /*color_core*/ color_hot,
                               /*color_outer*/ color_cold,
                               /*time_offset*/ p.phase, /*style*/ style,
                               /*blend*/ blend);
        }
    }

    // ── 产出：体积团命令（VolumetricCommand，供"真 3D 体积"渲染）──
    // 每颗粒子 = 一个**体积团**：把 2D billboard 升级成世界空间 ray-march 的
    // 3D 密度场。参数与 AppendParticles 同源（size/heat/相位），另给旋涡强度。
    void AppendVolumetric(RenderCommandList* cmds, const Color& color_hot,
                          const Color& color_cold, float intensity,
                          float noise_freq, int octaves, float density_threshold,
                          float swirl, ParticleBlend blend) const {
        for (const FireParticleState& p : pool_) {
            if (!p.alive) {
                continue;
            }
            const float t = p.age / p.life;
            const float size_mul = 1.0f + (p.grow - 1.0f) * std::sin(t * 3.14159f);
            const float radius = p.size * size_mul;
            const float heat = std::max(0.0f, 1.0f - 0.95f * t);
            const float fade = std::sin(std::min(1.0f, t * 1.15f) * 3.14159f);
            const float alpha = std::max(0.0f, fade) * (0.55f + 0.45f * heat);
            cmds->DrawVolumetric(/*center*/ p.pos, /*radius*/ radius,
                                 /*heat*/ heat, /*noise_freq*/ noise_freq,
                                 /*octaves*/ octaves,
                                 /*density_threshold*/ density_threshold,
                                 /*swirl*/ swirl,
                                 /*intensity*/ alpha * intensity,
                                 /*color_core*/ color_hot,
                                 /*color_outer*/ color_cold,
                                 /*time_offset*/ p.phase, /*blend*/ blend);
        }
    }

    // ── 产出：旧接口（FireCommand，供 fire/burning 旧路径）──
    void Append(RenderCommandList* cmds, const Color& color_hot,
                const Color& color_cold, float intensity, ParticleBlend blend,
                float noise_scale = 3.4f) const {
        for (const FireParticleState& p : pool_) {
            if (!p.alive) {
                continue;
            }
            const float t = p.age / p.life;
            const float size_mul = 1.0f + (p.grow - 1.0f) * std::sin(t * 3.14159f);
            const float half_w = p.size * size_mul;
            const Color c = LerpColor(color_hot, color_cold, t);
            const float fade = (1.0f - t) * (1.0f - t);
            cmds->DrawFire(p.pos, half_w, half_w * 2.4f, c, c,
                           intensity * (0.35f + 0.65f * fade), p.speed,
                           noise_scale, blend, p.phase);
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
    static ParticleStyle ToStyle(Mode m) {
        switch (m) {
            case Mode::kSoftPuff:
                return ParticleStyle::kSoftPuff;
            case Mode::kTongue:
                return ParticleStyle::kTongue;
            case Mode::kVortex:
                return ParticleStyle::kVortex;
        }
        return ParticleStyle::kSoftPuff;
    }

    static Vec3f Damp(const Vec3f& v, float drag, float dt) {
        const float k = 1.0f / (1.0f + drag * dt);
        return Vec3f(v.x() * k, v.y() * (1.0f / (1.0f + drag * 0.35f * dt)), v.z() * k);
    }

    FireParticleState* Acquire() {
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
    // 3D 值噪声（格点哈希 + 三线性插值）。
    static float Noise3(const Vec3f& p) {
        const float xf = std::floor(p.x()), yf = std::floor(p.y()), zf = std::floor(p.z());
        const float fx = p.x() - xf, fy = p.y() - yf, fz = p.z() - zf;
        const float ux = fx * fx * (3.0f - 2.0f * fx);
        const float uy = fy * fy * (3.0f - 2.0f * fy);
        const float uz = fz * fz * (3.0f - 2.0f * fz);
        auto lattice = [&](int dx, int dy, int dz) {
            const uint32_t h = HashMix(static_cast<uint32_t>(static_cast<int>(xf) + dx) * 73856093u ^
                                       static_cast<uint32_t>(static_cast<int>(yf) + dy) * 19349663u ^
                                       static_cast<uint32_t>(static_cast<int>(zf) + dz) * 83492791u);
            return static_cast<float>(h >> 8) / 16777216.0f;
        };
        const float c000 = lattice(0, 0, 0), c100 = lattice(1, 0, 0);
        const float c010 = lattice(0, 1, 0), c110 = lattice(1, 1, 0);
        const float c001 = lattice(0, 0, 1), c101 = lattice(1, 0, 1);
        const float c011 = lattice(0, 1, 1), c111 = lattice(1, 1, 1);
        const float x00 = c000 + (c100 - c000) * ux;
        const float x10 = c010 + (c110 - c010) * ux;
        const float x01 = c001 + (c101 - c001) * ux;
        const float x11 = c011 + (c111 - c011) * ux;
        const float y0 = x00 + (x10 - x00) * uy;
        const float y1 = x01 + (x11 - x01) * uy;
        return y0 + (y1 - y0) * uz;   // 0..1
    }
    // 3D 向量势 Ψ（三个标量噪声），curl = ∇×Ψ → 无散度旋涡场。
    static Vec3f Potential(const Vec3f& p) {
        return Vec3f(Noise3(p),
                     Noise3(Vec3f(p.x() + 31.4f, p.y() + 11.7f, p.z() + 7.3f)),
                     Noise3(Vec3f(p.x() + 73.1f, p.y() + 53.9f, p.z() + 3.1f)));
    }
    static Vec3f CurlNoise(const Vec3f& p, float t) {
        const float tt = 0.31f * t;                 // 场随时间缓慢演化
        const Vec3f q(p.x(), p.y() - tt, p.z());
        const float e = 0.35f;
        const Vec3f a1 = Potential(q + Vec3f(0, 0, e));
        const Vec3f a2 = Potential(q - Vec3f(0, 0, e));
        const Vec3f b1 = Potential(q + Vec3f(0, e, 0));
        const Vec3f b2 = Potential(q - Vec3f(0, e, 0));
        const Vec3f c1 = Potential(q + Vec3f(e, 0, 0));
        const Vec3f c2 = Potential(q - Vec3f(e, 0, 0));
        const float inv = 1.0f / (2.0f * e);
        return Vec3f(((b1.z() - b2.z()) - (a1.y() - a2.y())) * inv,
                     ((a1.x() - a2.x()) - (c1.z() - c2.z())) * inv,
                     ((c1.y() - c2.y()) - (b1.x() - b2.x())) * inv);
    }
    static Color LerpColor(const Color& a, const Color& b, float t) {
        const float tc = std::max(0.0f, std::min(1.0f, t));
        return Color{a.r + (b.r - a.r) * tc, a.g + (b.g - a.g) * tc,
                     a.b + (b.b - a.b) * tc, a.a + (b.a - a.a) * tc};
    }

    std::vector<FireParticleState> pool_;
    size_t next_ = 0;
    Mode mode_ = Mode::kSoftPuff;
    float rate_ = 300.0f;
    float acc_ = 0.0f;
    uint32_t emit_seed_ = 1;
    size_t emitted_ = 0;
};

}  // namespace jpov

#endif  // JPOV_EFFECT_PARTICLE_FIRE_FIRE_PARTICLES_H_
