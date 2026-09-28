// JPOV FireRenderer 实现 —— 火焰 billboard 顶点展开 + 绘制。
//
// GL 头文件必须最先 include（在 MinGW #define 宏替换之前），与 primitives3d_renderer.cc 同构。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/effect/fire_render/fire_renderer.h"

#include <cmath>

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
// MinGW 的 windows.h 定义了 near/far 宏，与 C++ 关键字 / 变量名冲突
#ifdef _WIN32
#undef near
#undef far
#endif

#include "third_party/gl_loader-mingw/gl_loader.h"
#endif

#include <glog/logging.h>

namespace jpov {

namespace {

// 一条火焰 quad 的顶点数：2 个三角形 = 6 顶点，每顶点 5 floats（pos.xyz + uv.xy）。
constexpr int kFireVertexFloats = 5;
constexpr int kFireVertexCount = 6;

}  // namespace

void FireRenderer::DrawFire(const FireCommand& cmd, const Camera& cam,
                            unsigned int stream_vbo, unsigned int prog,
                            const float mvp[16], float time) {
    // 圆柱 billboard 的水平右向量：right = normalize(cross(worldUp, 水平视线))。
    // 视线取「相机 → 火焰」的水平投影；退化（火焰正上方/正下方）时回退到 +X。
    const Vec3f to_fire = cmd.base - cam.position;
    const float hlen = std::sqrt(to_fire.x() * to_fire.x() + to_fire.z() * to_fire.z());
    Vec3f right;
    if (hlen < 1e-6f) {
        right = Vec3f(1.0f, 0.0f, 0.0f);
    } else {
        // cross((0,1,0), (hx,0,hz)) = (hz, 0, -hx)，再归一化（hlen 已是长度分子）。
        right = Vec3f(to_fire.z() / hlen, 0.0f, -to_fire.x() / hlen);
    }

    const Vec3f rx = right * cmd.radius;
    const Vec3f up(0.0f, cmd.height, 0.0f);
    const Vec3f bl = cmd.base - rx;              // 左下（底部中心 - right*radius）
    const Vec3f br = cmd.base + rx;              // 右下
    const Vec3f tl = bl + up;                    // 左上
    const Vec3f tr = br + up;                    // 右上

    // 2 个三角形：bl-br-tr、bl-tr-tl。UV（v: 底 0 → 顶 1）。
    const float verts[kFireVertexCount * kFireVertexFloats] = {
        bl.x(), bl.y(), bl.z(), 0.0f, 0.0f,
        br.x(), br.y(), br.z(), 1.0f, 0.0f,
        tr.x(), tr.y(), tr.z(), 1.0f, 1.0f,
        bl.x(), bl.y(), bl.z(), 0.0f, 0.0f,
        tr.x(), tr.y(), tr.z(), 1.0f, 1.0f,
        tl.x(), tl.y(), tl.z(), 0.0f, 1.0f,
    };

    glUseProgram(prog);
    glUniformMatrix4fv(glGetUniformLocation(prog, "uMVP"), 1, GL_FALSE, mvp);
    glUniform1f(glGetUniformLocation(prog, "uTime"), time);
    glUniform1f(glGetUniformLocation(prog, "uSpeed"), cmd.speed);
    glUniform1f(glGetUniformLocation(prog, "uNoiseScale"), cmd.noise_scale);
    glUniform3f(glGetUniformLocation(prog, "uColorCore"),
                cmd.color_core.r, cmd.color_core.g, cmd.color_core.b);
    glUniform3f(glGetUniformLocation(prog, "uColorOuter"),
                cmd.color_outer.r, cmd.color_outer.g, cmd.color_outer.b);
    glUniform1f(glGetUniformLocation(prog, "uIntensity"), cmd.intensity);

    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                          kFireVertexFloats * sizeof(float),
                          reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE,
                          kFireVertexFloats * sizeof(float),
                          reinterpret_cast<void*>(3 * sizeof(float)));
    glDrawArrays(GL_TRIANGLES, 0, kFireVertexCount);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

}  // namespace jpov
