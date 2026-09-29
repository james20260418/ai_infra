// JPOV ParticleRenderer 实现。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/effect/particle_fire/particle_renderer.h"

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
#undef near
#undef far
#include "third_party/gl_loader-mingw/gl_loader.h"
#endif

#include <glog/logging.h>

namespace jpov {

void ParticleRenderer::DrawParticleQuad(const ParticleCommand& cmd,
                                        const Camera& cam,
                                        unsigned int stream_vbo, unsigned int prog,
                                        const float mvp[16], float time) {
    // 圆柱 billboard 右向量（与 fire/burning 同构）。
    const Vec3f to_p = cmd.position - cam.position;
    const float hlen = std::sqrt(to_p.x() * to_p.x() + to_p.z() * to_p.z());
    Vec3f right;
    if (hlen < 1e-6f) {
        right = Vec3f(1.0f, 0.0f, 0.0f);
    } else {
        right = Vec3f(to_p.z() / hlen, 0.0f, -to_p.x() / hlen);
    }
    const float half_w = 0.5f * cmd.size;
    const float height = cmd.size * cmd.aspect;
    const Vec3f rx = right * half_w;
    // 竖直：以 position 为底边中点（火苗从底部往上长）。
    const Vec3f bl = cmd.position - rx;
    const Vec3f br = cmd.position + rx;
    const Vec3f tl = bl + Vec3f(0.0f, height, 0.0f);
    const Vec3f tr = br + Vec3f(0.0f, height, 0.0f);
    const float verts[6 * 5] = {
        bl.x(), bl.y(), bl.z(), 0.0f, 0.0f,
        br.x(), br.y(), br.z(), 1.0f, 0.0f,
        tr.x(), tr.y(), tr.z(), 1.0f, 1.0f,
        bl.x(), bl.y(), bl.z(), 0.0f, 0.0f,
        tr.x(), tr.y(), tr.z(), 1.0f, 1.0f,
        tl.x(), tl.y(), tl.z(), 0.0f, 1.0f,
    };
    glUseProgram(prog);
    glUniformMatrix4fv(glGetUniformLocation(prog, "uMVP"), 1, GL_FALSE, mvp);
    glUniform1f(glGetUniformLocation(prog, "uTime"), time + cmd.time_offset);
    glUniform1f(glGetUniformLocation(prog, "uHeat"), cmd.heat);
    glUniform1f(glGetUniformLocation(prog, "uAlpha"), cmd.alpha);
    glUniform1i(glGetUniformLocation(prog, "uStyle"),
                static_cast<int>(cmd.style));
    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                          reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                          reinterpret_cast<void*>(3 * sizeof(float)));
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

}  // namespace jpov
