// JPOV BurningRenderer 实现 —— 「燃烧体」表面火舌采样 + 绘制。
//
// GL 头文件必须最先 include（在 MinGW #define 宏替换之前），与 primitives3d 同构。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/effect/burning_render/burning_renderer.h"

#include <cmath>
#include <cstdint>

#ifdef _WIN32
#include <GL/gl.h>
#else
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#ifdef _WIN32
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
#undef near
#undef far
#include "third_party/gl_loader-mingw/gl_loader.h"
#endif

#include <glog/logging.h>

namespace jpov {

namespace {

// 一条 quad 的顶点数：2 个三角形 = 6 顶点，每顶点 5 floats（pos.xyz + uv.xy）。
constexpr int kQuadVertexFloats = 5;
constexpr int kQuadVertexCount  = 6;

// 确定性哈希（splitmix64 风格），供火源采样用（不用 rand()，保证可复现）。
inline uint32_t HashMix(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
// 把哈希转成 [0,1) 浮点。
inline float HashFloat(uint32_t h) {
    return static_cast<float>(h >> 8) / 16777216.0f;   // 取高 24 位
}

// 画一束 billboard quad（世界坐标 + UV，底 0 → 顶 1）。
void DrawBillboard(unsigned int stream_vbo, unsigned int prog,
                   const Camera& cam, const Vec3f& base, float half_w,
                   float height, const float mvp[16]) {
    const Vec3f to_obj = base - cam.position;
    const float hlen = std::sqrt(to_obj.x() * to_obj.x() + to_obj.z() * to_obj.z());
    Vec3f right;
    if (hlen < 1e-6f) {
        right = Vec3f(1.0f, 0.0f, 0.0f);
    } else {
        right = Vec3f(to_obj.z() / hlen, 0.0f, -to_obj.x() / hlen);
    }
    const Vec3f rx = right * half_w;
    const Vec3f up(0.0f, height, 0.0f);
    const Vec3f bl = base - rx;
    const Vec3f br = base + rx;
    const Vec3f tl = bl + up;
    const Vec3f tr = br + up;
    const float verts[kQuadVertexCount * kQuadVertexFloats] = {
        bl.x(), bl.y(), bl.z(), 0.0f, 0.0f,
        br.x(), br.y(), br.z(), 1.0f, 0.0f,
        tr.x(), tr.y(), tr.z(), 1.0f, 1.0f,
        bl.x(), bl.y(), bl.z(), 0.0f, 0.0f,
        tr.x(), tr.y(), tr.z(), 1.0f, 1.0f,
        tl.x(), tl.y(), tl.z(), 0.0f, 1.0f,
    };
    glUseProgram(prog);
    glUniformMatrix4fv(glGetUniformLocation(prog, "uMVP"), 1, GL_FALSE, mvp);
    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                          kQuadVertexFloats * sizeof(float),
                          reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE,
                          kQuadVertexFloats * sizeof(float),
                          reinterpret_cast<void*>(3 * sizeof(float)));
    glDrawArrays(GL_TRIANGLES, 0, kQuadVertexCount);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

}  // namespace

std::vector<Vec3f> BurningRenderer::SampleBurningPoints(
    const Vec3f& base, const Vec3f& up, const Vec3f& front,
    const Vec3f& half_extents, float strength, uint32_t seed) {
    std::vector<Vec3f> pts;
    // 正交基（与 Object3DCommand 的 up/front 处理一致）：n=front, u=up, r=cross(u,n)。
    Vec3f n = front;
    const float nl = std::sqrt(n.x() * n.x() + n.y() * n.y() + n.z() * n.z());
    CHECK_GT(nl, 1e-8f) << "SampleBurningPoints: front 退化";
    n = Vec3f(n.x() / nl, n.y() / nl, n.z() / nl);
    Vec3f u = up;
    const float dot_un = u.x() * n.x() + u.y() * n.y() + u.z() * n.z();
    u = Vec3f(u.x() - dot_un * n.x(), u.y() - dot_un * n.y(), u.z() - dot_un * n.z());
    const float ul = std::sqrt(u.x() * u.x() + u.y() * u.y() + u.z() * u.z());
    CHECK_GT(ul, 1e-8f) << "SampleBurningPoints: up 与 front 共线";
    u = Vec3f(u.x() / ul, u.y() / ul, u.z() / ul);
    const Vec3f r(u.y() * n.z() - u.z() * n.y(),
                  u.z() * n.x() - u.x() * n.z(),
                  u.x() * n.y() - u.y() * n.x());

    const float s = std::max(0.0f, std::min(1.0f, strength));
    // 底部一圈：固定 6 簇（常燃根部）。
    const int bottom_count = 6;
    for (int i = 0; i < bottom_count; ++i) {
        const float a = 6.2831853071795864769f * static_cast<float>(i) /
                        static_cast<float>(bottom_count);
        // 略出于物体表面（贴表面外一点点）。
        const Vec3f p = base + r * ((half_extents.x() + 0.02f) * std::cos(a)) +
                        n * ((half_extents.z() + 0.02f) * std::sin(a));
        pts.push_back(p);
    }

    // 沿竖直往上蔓延：簇数随强度增加（越高越稀）。
    const int climb_max = static_cast<int>(std::round(2.0f + 6.0f * s));
    for (int i = 0; i < climb_max; ++i) {
        const uint32_t h = HashMix(seed * 2654435761u + static_cast<uint32_t>(i) * 40503u);
        const float fy = HashFloat(h);                       // 0..1 竖直位置
        // 高度用平方分布 → 越往上越稀。
        const float ty = fy * fy;
        const uint32_t h2 = HashMix(h);
        const float side = HashFloat(HashMix(h2));           // 选哪个侧面
        const float off  = (HashFloat(HashMix(h2 * 7919u)) - 0.5f);  // 表面上的偏移
        // 侧向偏移：在 r 方向（±）或 n 方向（±）。
        Vec3f lateral;
        if (side < 0.5f) {
            const float sgn = (side < 0.25f) ? 1.0f : -1.0f;
            lateral = r * (sgn * (half_extents.x() + 0.02f)) +
                      n * (off * 2.0f * half_extents.z());
        } else {
            const float sgn = (side < 0.75f) ? 1.0f : -1.0f;
            lateral = n * (sgn * (half_extents.z() + 0.02f)) +
                      r * (off * 2.0f * half_extents.x());
        }
        const Vec3f p = base + u * (ty * 2.0f * half_extents.y()) + lateral;
        pts.push_back(p);
    }
    return pts;
}

void BurningRenderer::DrawBurning(const BurningCommand& cmd, const Camera& cam,
                                  unsigned int stream_vbo,
                                  unsigned int fire_prog, unsigned int smoke_prog,
                                  const float mvp[16], float time) {
    CHECK_GT(cmd.half_extents.x(), 0.0f) << "DrawBurning: half_extents.x 必须 > 0";
    CHECK_GT(cmd.half_extents.y(), 0.0f) << "DrawBurning: half_extents.y 必须 > 0";
    CHECK_GT(cmd.half_extents.z(), 0.0f) << "DrawBurning: half_extents.z 必须 > 0";
    CHECK_GE(cmd.strength, 0.0f) << "DrawBurning: strength 必须 >= 0";

    // 物体底面中心 = center - up*half_y（火源由底面往上长）。
    const float ul = std::sqrt(cmd.up.x() * cmd.up.x() + cmd.up.y() * cmd.up.y() +
                               cmd.up.z() * cmd.up.z());
    const Vec3f up_n = (ul > 1e-8f)
        ? Vec3f(cmd.up.x() / ul, cmd.up.y() / ul, cmd.up.z() / ul)
        : Vec3f(0.0f, 1.0f, 0.0f);
    const Vec3f base = cmd.center - up_n * cmd.half_extents.y();

    const std::vector<Vec3f> pts = SampleBurningPoints(
        base, cmd.up, cmd.front, cmd.half_extents, cmd.strength, cmd.seed);

    // ── ① 火舌 ──
    for (size_t i = 0; i < pts.size(); ++i) {
        const float rnd = HashFloat(HashMix(cmd.seed * 2246822519u +
                                            static_cast<uint32_t>(i)));
        const float half_w = 0.10f + 0.06f * rnd;
        const float height = (0.35f + 0.35f * rnd) * (0.6f + 0.6f * cmd.strength);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);   // 火：alpha
        glUseProgram(fire_prog);
        glUniform1f(glGetUniformLocation(fire_prog, "uTime"),
                    time + rnd * 10.0f);                      // 各簇相位错开
        glUniform1f(glGetUniformLocation(fire_prog, "uSpeed"), cmd.speed);
        glUniform1f(glGetUniformLocation(fire_prog, "uNoiseScale"), 3.0f);
        glUniform3f(glGetUniformLocation(fire_prog, "uColorCore"),
                    cmd.color_core.r, cmd.color_core.g, cmd.color_core.b);
        glUniform3f(glGetUniformLocation(fire_prog, "uColorOuter"),
                    cmd.color_outer.r, cmd.color_outer.g, cmd.color_outer.b);
        glUniform1f(glGetUniformLocation(fire_prog, "uIntensity"), cmd.intensity);
        glUniform1f(glGetUniformLocation(fire_prog, "uDensity"), 0.45f);
        DrawBillboard(stream_vbo, fire_prog, cam, pts[i], half_w, height, mvp);
    }

    // ── ② 烟（同一批点，上方飘起；alpha 混合）──
    for (size_t i = 0; i < pts.size(); ++i) {
        const float rnd = HashFloat(HashMix(cmd.seed * 3266489917u +
                                            static_cast<uint32_t>(i)));
        // 只给一部分点加烟（不是每簇都冒烟）。
        if (rnd < 0.45f) {
            continue;
        }
        const Vec3f p = pts[i] + Vec3f(0.0f, 0.45f, 0.0f);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(smoke_prog);
        glUniform1f(glGetUniformLocation(smoke_prog, "uTime"), time + rnd * 7.0f);
        glUniform1f(glGetUniformLocation(smoke_prog, "uSpeed"), cmd.speed * 0.6f);
        glUniform1f(glGetUniformLocation(smoke_prog, "uNoiseScale"), 2.4f);
        glUniform4f(glGetUniformLocation(smoke_prog, "uColor"),
                    cmd.smoke_color.r, cmd.smoke_color.g, cmd.smoke_color.b,
                    cmd.smoke_color.a);
        glUniform1f(glGetUniformLocation(smoke_prog, "uIntensity"), 1.0f);
        DrawBillboard(stream_vbo, smoke_prog, cam, p, 0.22f, 0.9f, mvp);
    }

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

}  // namespace jpov
