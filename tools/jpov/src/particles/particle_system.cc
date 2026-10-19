// JPOV 广告牌粒子火 — 模拟内核实现。见 particle_system.h。

#include "tools/jpov/src/particles/particle_system.h"

#include <algorithm>
#include <cmath>

#include <glog/logging.h>

namespace jpov {
namespace particles {

namespace {

constexpr float kPi = 3.14159265358979323846f;
// 存活粒子上限（防御失控；rate 与 lifetime 之积正常应远小于此）。
constexpr int kMaxLiveParticles = 20000;

// 叉积（geom::Vec3 无成员 Cross，自行实现）。
Vec3f Cross(const Vec3f& a, const Vec3f& b) {
    return Vec3f(a.y() * b.z() - a.z() * b.y(),
                 a.z() * b.x() - a.x() * b.z(),
                 a.x() * b.y() - a.y() * b.x());
}

// 由单位轴 n 构造与之正交的归一化基 (t, b)。
void OrthonormalBasis(const Vec3f& n, Vec3f* t /*output*/, Vec3f* b /*output*/) {
    const Vec3f helper = (std::fabs(n.y()) < 0.99f) ? Vec3f(0.0f, 1.0f, 0.0f)
                                                    : Vec3f(1.0f, 0.0f, 0.0f);
    *t = Cross(helper, n).Unit();
    *b = Cross(n, *t);
}

float Clamp01(float v) {
    return (v < 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v);
}

}  // namespace

// ==================== Rng ====================

uint64_t Rng::NextU64() {
    state_ += 0x9E3779B97F4A7C15ull;
    uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

float Rng::Next01() {
    // 取高 24 位映射到 [0,1)：精度足够且不与低位的周期结构耦合。
    return static_cast<float>(NextU64() >> 40) * (1.0f / 16777216.0f);
}

float Rng::Range(float lo, float hi) {
    if (hi <= lo) {
        return lo;
    }
    return lo + (hi - lo) * Next01();
}

// ==================== RangeF ====================

float RangeF::Sample(Rng& rng) const {
    CHECK(max >= min) << "RangeF: max<min (" << max << "<" << min << ")";
    return rng.Range(min, max);
}

// ==================== Curve ====================

float Curve::Eval(float t) const {
    if (keys.empty()) {
        return 0.0f;
    }
    if (t <= keys.front().t) {
        return keys.front().v;
    }
    if (t >= keys.back().t) {
        return keys.back().v;
    }
    for (size_t i = 1; i < keys.size(); ++i) {
        if (t <= keys[i].t) {
            const float t0 = keys[i - 1].t;
            const float t1 = keys[i].t;
            const float span = (t1 > t0) ? (t1 - t0) : 1.0f;
            const float f = (t - t0) / span;
            return keys[i - 1].v + (keys[i].v - keys[i - 1].v) * f;
        }
    }
    return keys.back().v;
}

// ==================== Gradient ====================

Color Gradient::Eval(float t) const {
    if (keys.empty()) {
        return Color{1.0f, 1.0f, 1.0f, 1.0f};
    }
    if (t <= keys.front().t) {
        return keys.front().c;
    }
    if (t >= keys.back().t) {
        return keys.back().c;
    }
    for (size_t i = 1; i < keys.size(); ++i) {
        if (t <= keys[i].t) {
            const float t0 = keys[i - 1].t;
            const float t1 = keys[i].t;
            const float span = (t1 > t0) ? (t1 - t0) : 1.0f;
            const float f = (t - t0) / span;
            const Color& a = keys[i - 1].c;
            const Color& b = keys[i].c;
            return Color{a.r + (b.r - a.r) * f,
                         a.g + (b.g - a.g) * f,
                         a.b + (b.b - a.b) * f,
                         a.a + (b.a - a.a) * f};
        }
    }
    return keys.back().c;
}

// ==================== 预设曲线 / 渐变 ====================

Gradient MakeDefaultFireGradient() {
    Gradient g;
    // 白热（出生）→ 黄 → 橙 → 红 → 暗红（末段）；alpha 两端低、中段高，末端淡出。
    g.keys = {
        {0.00f, Color{1.00f, 0.95f, 0.75f, 0.50f}},
        {0.15f, Color{1.00f, 0.80f, 0.30f, 1.00f}},
        {0.45f, Color{1.00f, 0.45f, 0.08f, 1.00f}},
        {0.75f, Color{0.75f, 0.13f, 0.02f, 0.85f}},
        {1.00f, Color{0.20f, 0.02f, 0.00f, 0.00f}},
    };
    return g;
}

Curve MakeDefaultFireSizeCurve() {
    Curve c;
    c.keys = {
        {0.00f, 0.35f},
        {0.30f, 1.00f},
        {0.70f, 0.95f},
        {1.00f, 0.45f},
    };
    return c;
}

Curve MakeDefaultLinearFrameCurve() {
    Curve c;
    c.keys = {
        {0.00f, 0.00f},
        {1.00f, 1.00f},
    };
    return c;
}

// ==================== EmitterConfig ====================

void EmitterConfig::Validate() const {
    CHECK_GE(rate, 0.0f) << "EmitterConfig.rate 必须 >= 0";
    CHECK_GE(burst, 0) << "EmitterConfig.burst 必须 >= 0";
    CHECK_GE(radius, 0.0f) << "EmitterConfig.radius 必须 >= 0";
    CHECK_GE(box_half.x(), 0.0f);
    CHECK_GE(box_half.y(), 0.0f);
    CHECK_GE(box_half.z(), 0.0f);
    CHECK_GT(size_start.max, 0.0f) << "EmitterConfig.size_start.max 必须 > 0";
    CHECK_GT(lifetime.max, 0.0f) << "EmitterConfig.lifetime.max 必须 > 0";
    CHECK_GE(tiles_x, 1) << "EmitterConfig.tiles_x 必须 >= 1";
    CHECK_GE(tiles_y, 1) << "EmitterConfig.tiles_y 必须 >= 1";
    CHECK_GE(cone_half_angle_deg, 0.0f);
    CHECK_LE(cone_half_angle_deg, 90.0f);
}

// ==================== ParticleEmitter ====================

ParticleEmitter::ParticleEmitter(EmitterConfig cfg) : cfg_(cfg) {
    cfg_.Validate();
}

void ParticleEmitter::Reset(uint32_t seed) {
    cfg_.Validate();
    rng_ = Rng(seed);
    particles_.clear();
    spawn_accumulator_ = 0.0f;
    primed_ = false;
}

void ParticleEmitter::SetConfig(const EmitterConfig& cfg) {
    cfg.Validate();
    cfg_ = cfg;
}

void ParticleEmitter::SampleEmission(Rng& rng, Vec3f* position /*output*/,
                                     Vec3f* direction /*output*/) {
    Vec3f dir = cfg_.emit_dir;
    const float dn = dir.Norm();
    dir = (dn > 1e-6f) ? Vec3f(dir.x() / dn, dir.y() / dn, dir.z() / dn)
                       : Vec3f(0.0f, 1.0f, 0.0f);

    Vec3f pos = cfg_.origin;
    switch (cfg_.shape) {
        case ShapeKind::kPoint:
            break;
        case ShapeKind::kCircle: {
            const float theta = rng.Next01() * 2.0f * kPi;
            const float r = cfg_.radius * std::sqrt(rng.Next01());
            pos = Vec3f(cfg_.origin.x() + r * std::cos(theta),
                        cfg_.origin.y(),
                        cfg_.origin.z() + r * std::sin(theta));
            break;
        }
        case ShapeKind::kCone: {
            Vec3f t;
            Vec3f b;
            OrthonormalBasis(dir, &t, &b);
            const float theta = rng.Next01() * 2.0f * kPi;
            const float r = cfg_.radius * std::sqrt(rng.Next01());
            Vec3f offset = t * (r * std::cos(theta)) + b * (r * std::sin(theta));
            pos = cfg_.origin + offset;
            // 方向：在 dir 周围 cone_half_angle 内均匀采样。
            const float cos_half = std::cos(cfg_.cone_half_angle_deg * kPi / 180.0f);
            const float cos_t = 1.0f - rng.Next01() * (1.0f - cos_half);
            const float sin_t = std::sqrt(std::max(0.0f, 1.0f - cos_t * cos_t));
            const float phi = rng.Next01() * 2.0f * kPi;
            dir = t * (sin_t * std::cos(phi)) + b * (sin_t * std::sin(phi)) +
                  dir * cos_t;
            const float n2 = dir.Norm();
            if (n2 > 1e-6f) {
                dir = Vec3f(dir.x() / n2, dir.y() / n2, dir.z() / n2);
            }
            break;
        }
        case ShapeKind::kBox: {
            pos = Vec3f(cfg_.origin.x() + rng.Range(-cfg_.box_half.x(), cfg_.box_half.x()),
                        cfg_.origin.y() + rng.Range(-cfg_.box_half.y(), cfg_.box_half.y()),
                        cfg_.origin.z() + rng.Range(-cfg_.box_half.z(), cfg_.box_half.z()));
            break;
        }
    }
    *position = pos;
    *direction = dir;
}

ParticleEmitter::Particle ParticleEmitter::Spawn() {
    Particle p;
    Vec3f dir;
    SampleEmission(rng_, &p.position, &dir);
    const float sp = cfg_.speed.Sample(rng_);
    p.velocity = dir * sp;
    p.lifetime = cfg_.lifetime.Sample(rng_);
    p.rotation0 = cfg_.rotation.Sample(rng_);
    p.size0 = cfg_.size_start.Sample(rng_);
    p.frame_phase = cfg_.start_frame_random ? rng_.Next01() : 0.0f;
    p.seed = static_cast<uint32_t>(rng_.NextU64());
    p.age = 0.0f;
    return p;
}

ParticleEmitter::Particle ParticleEmitter::MakeParticle(const Vec3f& position,
                                                        const Vec3f& velocity) {
    Particle p;
    p.position = position;
    p.velocity = velocity;
    p.lifetime = cfg_.lifetime.Sample(rng_);
    p.rotation0 = cfg_.rotation.Sample(rng_);
    p.size0 = cfg_.size_start.Sample(rng_);
    p.frame_phase = cfg_.start_frame_random ? rng_.Next01() : 0.0f;
    p.seed = static_cast<uint32_t>(rng_.NextU64());
    p.age = 0.0f;
    return p;
}

void ParticleEmitter::SpawnAt(const Vec3f& position) {
    if (static_cast<int>(particles_.size()) >= kMaxLiveParticles) {
        return;
    }
    Vec3f dir = cfg_.emit_dir;
    const float dn = dir.Norm();
    dir = (dn > 1e-6f) ? Vec3f(dir.x() / dn, dir.y() / dn, dir.z() / dn)
                       : Vec3f(0.0f, 1.0f, 0.0f);
    const float sp = cfg_.speed.Sample(rng_);
    // 少量水平抖动，避免烟柱一条直线。
    const float jx = (rng_.Next01() - 0.5f) * 0.4f;
    const float jz = (rng_.Next01() - 0.5f) * 0.4f;
    particles_.push_back(MakeParticle(position, dir * sp + Vec3f(jx, 0.0f, jz)));
}

float ParticleEmitter::ResolveFrame(const Particle& p) const {
    if (!cfg_.flipbook) {
        return 0.0f;
    }
    const int total = cfg_.tiles_x * cfg_.tiles_y;
    const float t = Clamp01(p.age / std::max(p.lifetime, 1e-6f));
    float phase = 0.0f;  // [0,1) 归一化帧相位
    switch (cfg_.sheet_time_mode) {
        case SheetTimeMode::kLifetime: {
            const float f = cfg_.frame_curve.Eval(t) * cfg_.sheet_cycles;
            phase = f - std::floor(f);
            break;
        }
        case SheetTimeMode::kFps: {
            const float f = p.age * cfg_.sheet_fps / static_cast<float>(total) +
                            p.frame_phase;
            phase = f - std::floor(f);
            break;
        }
        case SheetTimeMode::kSpeed: {
            const float sp = p.velocity.Norm();
            const float f = sp * 0.5f + p.frame_phase;
            phase = f - std::floor(f);
            break;
        }
        case SheetTimeMode::kPairMorph: {
            const int plen = (cfg_.pair_len < 2) ? 2 : cfg_.pair_len;
            const int pairs = std::max(1, total / plen);
            int k = static_cast<int>(p.frame_phase * static_cast<float>(pairs));
            k = (k < 0) ? 0 : (k >= pairs ? pairs - 1 : k);
            const float f = static_cast<float>(k * plen) +
                            t * static_cast<float>(plen - 1);
            const float fmax = static_cast<float>(total - 1);
            return (f > fmax) ? fmax : f;
        }
    }
    float frame = phase * static_cast<float>(total);   // 分数帧：shader 做相邻帧混合（减卡顿/突变）
    if (frame >= static_cast<float>(total)) {
        frame = static_cast<float>(total - 1);
    }
    return frame;
}

void ParticleEmitter::Update(float dt, std::vector<ParticleInstance>* out /*output*/,
                             std::vector<Vec3f>* deaths /*output*/) {
    CHECK(out != nullptr) << "ParticleEmitter::Update: out 不能为空";
    CHECK_GE(dt, 0.0f) << "ParticleEmitter::Update: dt 必须 >= 0";

    if (!primed_) {
        const int n = cfg_.burst;
        for (int i = 0; i < n && static_cast<int>(particles_.size()) < kMaxLiveParticles; ++i) {
            particles_.push_back(Spawn());
        }
        primed_ = true;
    }

    // 按速率补发（累加器保证平均发射率精确）。
    spawn_accumulator_ += cfg_.rate * dt;
    while (spawn_accumulator_ >= 1.0f) {
        spawn_accumulator_ -= 1.0f;
        if (static_cast<int>(particles_.size()) >= kMaxLiveParticles) {
            spawn_accumulator_ = 0.0f;
            break;
        }
        particles_.push_back(Spawn());
    }

    // 积分 + 回收 + 输出。
    std::vector<Particle> alive;
    alive.reserve(particles_.size());
    for (Particle& p : particles_) {
        p.age += dt;
        if (p.age >= p.lifetime) {
            if (deaths != nullptr) {
                deaths->push_back(p.position);
            }
            continue;
        }

        Vec3f accel = cfg_.gravity;
        if (cfg_.noise_enabled && cfg_.noise_amp > 0.0f) {
            // 廉价确定性湍流：位置哈希出的方向场（逐粒子相位偏移）。
            const float ph = static_cast<float>(p.seed % 1024u) * 0.00613f;
            const Vec3f q = p.position * cfg_.noise_freq;
            const float nx = std::sin(q.x() * 12.9898f + q.y() * 78.2330f +
                                      q.z() * 37.7190f + ph);
            const float ny = std::sin(q.y() * 39.3468f + q.z() * 11.1350f +
                                      q.x() * 83.1550f + ph * 1.3f);
            const float nz = std::sin(q.z() * 73.1560f + q.x() * 52.2350f +
                                      q.y() * 9.1510f + ph * 1.7f);
            accel = accel + Vec3f(nx, ny, nz) * cfg_.noise_amp;
        }

        p.velocity = p.velocity + accel * dt;
        const float damp = 1.0f - cfg_.drag * dt;
        p.velocity = p.velocity * ((damp < 0.0f) ? 0.0f : damp);
        p.position = p.position + p.velocity * dt;

        const float t = Clamp01(p.age / std::max(p.lifetime, 1e-6f));

        float size = p.size0;
        if (cfg_.use_size_over_life) {
            size = p.size0 * cfg_.size_curve.Eval(t);
        }

        Color color;
        if (cfg_.use_color_over_life) {
            color = cfg_.color_over_life.Eval(t);
        } else {
            color = cfg_.color_start;
            color.a = cfg_.color_start.a * (1.0f - t);
        }

        ParticleInstance inst;
        inst.position = p.position;
        inst.velocity = p.velocity;
        inst.width = size;
        inst.height = size;
        inst.rotation = p.rotation0;
        inst.color = color;
        inst.frame = ResolveFrame(p);
        inst.age01 = t;
        out->push_back(inst);

        alive.push_back(p);
    }
    particles_.swap(alive);
}

}  // namespace particles
}  // namespace jpov
