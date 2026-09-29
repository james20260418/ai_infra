// JPOV VolumetricRenderer 实现。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/effect/volumetric_render/volumetric_renderer.h"

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

namespace {

geom::Vec3<float> Sub(const geom::Vec3<float>& a, const geom::Vec3<float>& b) {
    return {a.x() - b.x(), a.y() - b.y(), a.z() - b.z()};
}
geom::Vec3<float> Cross(const geom::Vec3<float>& a, const geom::Vec3<float>& b) {
    return {a.y() * b.z() - a.z() * b.y(),
            a.z() * b.x() - a.x() * b.z(),
            a.x() * b.y() - a.y() * b.x()};
}
geom::Vec3<float> Normalize(const geom::Vec3<float>& v) {
    const float len = std::sqrt(v.x() * v.x() + v.y() * v.y() + v.z() * v.z());
    if (len < 1e-8f) {
        return {0.0f, 0.0f, 0.0f};
    }
    return {v.x() / len, v.y() / len, v.z() / len};
}

}  // namespace

void VolumetricRenderer::DrawVolumetric(const VolumetricCommand& cmd,
                                        const Camera& cam,
                                        unsigned int stream_vbo, unsigned int prog,
                                        const float mvp[16], int fbo_w, int fbo_h,
                                        unsigned int scene_depth_tex, float time) {
    // 相机基（世界空间）：前 / 右 / 上。
    const geom::Vec3<float> fwd = Normalize(Sub(cam.target, cam.position));
    geom::Vec3<float> right = Normalize(Cross(fwd, cam.up));
    if (right.x() == 0.0f && right.y() == 0.0f && right.z() == 0.0f) {
        right = {1.0f, 0.0f, 0.0f};   // 视线与 up 平行时的退化兜底
    }
    const geom::Vec3<float> upv = Cross(right, fwd);

    // 完整朝向相机的包围盒 quad（边长 ~2.5R，留点余量给球投影）。
    const float e = cmd.radius * 1.25f;
    const geom::Vec3<float>& c = cmd.center;
    const geom::Vec3<float> rx{right.x() * e, right.y() * e, right.z() * e};
    const geom::Vec3<float> uy{upv.x() * e, upv.y() * e, upv.z() * e};
    const geom::Vec3<float> bl{c.x() - rx.x() - uy.x(), c.y() - rx.y() - uy.y(),
                               c.z() - rx.z() - uy.z()};
    const geom::Vec3<float> br{c.x() + rx.x() - uy.x(), c.y() + rx.y() - uy.y(),
                               c.z() + rx.z() - uy.z()};
    const geom::Vec3<float> tr{c.x() + rx.x() + uy.x(), c.y() + rx.y() + uy.y(),
                               c.z() + rx.z() + uy.z()};
    const geom::Vec3<float> tl{c.x() - rx.x() + uy.x(), c.y() - rx.y() + uy.y(),
                               c.z() - rx.z() + uy.z()};
    const geom::Vec3<float> tri[6] = {bl, br, tr, bl, tr, tl};
    float verts[6 * 3];
    for (int i = 0; i < 6; ++i) {
        verts[i * 3 + 0] = tri[i].x();
        verts[i * 3 + 1] = tri[i].y();
        verts[i * 3 + 2] = tri[i].z();
    }

    glUseProgram(prog);
    glUniformMatrix4fv(glGetUniformLocation(prog, "uVP"), 1, GL_FALSE, mvp);
    glUniform3f(glGetUniformLocation(prog, "uCamPos"),
                cam.position.x(), cam.position.y(), cam.position.z());
    glUniform3f(glGetUniformLocation(prog, "uCenter"), c.x(), c.y(), c.z());
    glUniform1f(glGetUniformLocation(prog, "uRadius"), cmd.radius);
    glUniform1f(glGetUniformLocation(prog, "uHeat"), cmd.heat);
    glUniform1f(glGetUniformLocation(prog, "uNoiseFreq"), cmd.noise_freq);
    glUniform1i(glGetUniformLocation(prog, "uOctaves"), cmd.octaves);
    glUniform1f(glGetUniformLocation(prog, "uDensityThreshold"), cmd.density_threshold);
    glUniform1f(glGetUniformLocation(prog, "uSwirl"), cmd.swirl);
    glUniform1f(glGetUniformLocation(prog, "uIntensity"), cmd.intensity);
    glUniform1f(glGetUniformLocation(prog, "uTime"), time + cmd.time_offset);
    glUniform3f(glGetUniformLocation(prog, "uColorCore"),
                cmd.color_core.r, cmd.color_core.g, cmd.color_core.b);
    glUniform3f(glGetUniformLocation(prog, "uColorOuter"),
                cmd.color_outer.r, cmd.color_outer.g, cmd.color_outer.b);
    glUniform2f(glGetUniformLocation(prog, "uResolution"),
                static_cast<float>(fbo_w), static_cast<float>(fbo_h));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scene_depth_tex);
    glUniform1i(glGetUniformLocation(prog, "uSceneDepth"), 0);

    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float),
                          reinterpret_cast<void*>(0));
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

}  // namespace jpov
