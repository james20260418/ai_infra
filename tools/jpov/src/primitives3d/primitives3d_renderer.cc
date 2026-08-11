// JPOV Primitives3DRenderer 实现
//
// 3D 图元绘制（三角形/条带/线段/文本）+ MVP 矩阵构建（纯 CPU）。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/primitives3d/primitives3d_renderer.h"

#include <cmath>

// GL 头文件必须最先 include（在 MinGW #define 宏替换之前）
#ifdef _WIN32
#include <GL/gl.h>
#else
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#ifdef _WIN32
// MinGW: windows.h 定义 ERROR 宏与 glog 冲突，必须在 glog 之前 suppress
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif

// MinGW 的 windows.h 定义了 near/far 宏，与 C++ 关键字 / 变量名冲突
#ifdef _WIN32
#undef near
#undef far
#endif

#include "third_party/gl_loader-mingw/gl_loader.h"

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#endif

#include <glog/logging.h>

namespace jpov {

namespace {
// GL_TRIANGLE_STRIP 顶点数上限（与先前 Renderer::kMaxStripVertices 一致）
constexpr int kMaxStripVertices = 3000;
}  // namespace

void Primitives3DRenderer::Mat4Mul(const float a[16], const float b[16],
                                   float out[16]) {
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a[k * 4 + row] * b[col * 4 + k];
            }
            out[col * 4 + row] = sum;
        }
    }
}

void Primitives3DRenderer::BuildPerspProj(float fov_y, float aspect, float near,
                                          float far, float out[16]) {
    float f = 1.0f / std::tan(fov_y * 0.5f);
    float range_inv = 1.0f / (near - far);
    // 列主序
    out[0]  = f / aspect;  out[4]  = 0.0f; out[8]  = 0.0f;                 out[12] = 0.0f;
    out[1]  = 0.0f;        out[5]  = f;     out[9]  = 0.0f;                 out[13] = 0.0f;
    out[2]  = 0.0f;        out[6]  = 0.0f;  out[10] = (near + far) * range_inv; out[14] = 2.0f * near * far * range_inv;
    out[3]  = 0.0f;        out[7]  = 0.0f;  out[11] = -1.0f;                out[15] = 0.0f;
}

void Primitives3DRenderer::BuildMVP(const Camera& cam, int fbo_w, int fbo_h,
                                    float mvp[16]) {
    float aspect = static_cast<float>(fbo_w) / static_cast<float>(fbo_h);
    float fov_rad = cam.fov * 3.14159265358979323846f / 180.0f;

    // === 投影矩阵 ===
    float proj[16];
    BuildPerspProj(fov_rad, aspect, cam.near, cam.far, proj);

    // === lookAt 视图矩阵 ===
    // 计算坐标基
    Vec3f fwd = cam.target - cam.position;
    float f_len = std::sqrt(fwd.x()*fwd.x() + fwd.y()*fwd.y() + fwd.z()*fwd.z());
    if (f_len < 1e-8f) { fwd = {0.0f, 0.0f, -1.0f}; }
    else { fwd = {fwd.x()/f_len, fwd.y()/f_len, fwd.z()/f_len}; }

    Vec3f side = {fwd.y()*cam.up.z() - fwd.z()*cam.up.y(),
                  fwd.z()*cam.up.x() - fwd.x()*cam.up.z(),
                  fwd.x()*cam.up.y() - fwd.y()*cam.up.x()};
    float s_len = std::sqrt(side.x()*side.x() + side.y()*side.y() + side.z()*side.z());
    if (s_len < 1e-8f) { side = {1.0f, 0.0f, 0.0f}; }
    else { side = {side.x()/s_len, side.y()/s_len, side.z()/s_len}; }

    Vec3f upv = {side.y()*fwd.z() - side.z()*fwd.y(),
                 side.z()*fwd.x() - side.x()*fwd.z(),
                 side.x()*fwd.y() - side.y()*fwd.x()};

    // 列主序 lookAt 矩阵 (OpenGL 右手系)
    float view[16] = {
        side.x(), upv.x(), -fwd.x(), 0.0f,
        side.y(), upv.y(), -fwd.y(), 0.0f,
        side.z(), upv.z(), -fwd.z(), 0.0f,
        -(side.x()*cam.position.x() + side.y()*cam.position.y() + side.z()*cam.position.z()),
        -(upv.x()*cam.position.x() + upv.y()*cam.position.y() + upv.z()*cam.position.z()),
         (fwd.x()*cam.position.x() + fwd.y()*cam.position.y() + fwd.z()*cam.position.z()),
         1.0f
    };

    // MVP = Proj * View
    Mat4Mul(proj, view, mvp);
}

void Primitives3DRenderer::BuildModelMatrix(const Vec3f& center,
                                            const Vec3f& up,
                                            const Vec3f& front,
                                            float model[16]) {
    // 归一化 up 与 front
    float u_len = std::sqrt(up.x()*up.x() + up.y()*up.y() + up.z()*up.z());
    float f_len = std::sqrt(front.x()*front.x() + front.y()*front.y() + front.z()*front.z());
    Vec3f upn = {up.x()/u_len, up.y()/u_len, up.z()/u_len};
    Vec3f frn = {front.x()/f_len, front.y()/f_len, front.z()/f_len};

    // right = normalize(cross(up, front))——保证模型右手系
    Vec3f right = {upn.y()*frn.z() - upn.z()*frn.y(),
                   upn.z()*frn.x() - upn.x()*frn.z(),
                   upn.x()*frn.y() - upn.y()*frn.x()};
    float r_len = std::sqrt(right.x()*right.x() + right.y()*right.y() + right.z()*right.z());
    right = {right.x()/r_len, right.y()/r_len, right.z()/r_len};

    // 列主序旋转部分：
    //   model[0..2] = right 列（世界 X 轴方向）
    //   model[4..6] = upn   列（世界 Y 轴方向）
    //   model[8..10]= frn   列（世界 Z 轴方向）
    // 平移在最后一列（12..14）
    model[0] = right.x(); model[4] = upn.x(); model[8]  = frn.x(); model[12] = center.x();
    model[1] = right.y(); model[5] = upn.y(); model[9]  = frn.y(); model[13] = center.y();
    model[2] = right.z(); model[6] = upn.z(); model[10] = frn.z(); model[14] = center.z();
    model[3] = 0.0f;      model[7] = 0.0f;    model[11] = 0.0f;    model[15] = 1.0f;
}

void Primitives3DRenderer::DrawTriangle3D(const Triangle3DCommand& cmd,
                                          unsigned int stream_vbo,
                                          unsigned int prog,
                                          const float mvp[16]) {
    // 3 个顶点 × xyz = 9 floats
    float verts[9] = {
        cmd.p1.x(), cmd.p1.y(), cmd.p1.z(),
        cmd.p2.x(), cmd.p2.y(), cmd.p2.z(),
        cmd.p3.x(), cmd.p3.y(), cmd.p3.z(),
    };

    glUseProgram(prog);
    glUniformMatrix4fv(glGetUniformLocation(prog, "uMVP"),
                       1, GL_FALSE, mvp);
    glUniform4f(glGetUniformLocation(prog, "uColor"),
                cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);

    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void Primitives3DRenderer::DrawStrip3D(const Strip3DCommand& cmd,
                                       unsigned int strip_vbo,
                                       unsigned int prog,
                                       const float mvp[16]) {
    int n = static_cast<int>(cmd.vertices.size());
    if (n < 3) return;

    // 截断到 3000 顶点上限
    int capped_n = (n > kMaxStripVertices) ? kMaxStripVertices : n;

    // 使用真正的 GL_TRIANGLE_STRIP，直接上传原始顶点序列。
    // GL_TRIANGLE_STRIP 的卷绕顺序为：三角形 i 由顶点 (i, i+1, i+2) 构成，
    // 每个三角形的卷绕方向取决于顶点索引的奇偶性——奇数三角形保持 CCW，
    // 偶数三角形自动反转卷绕以维持面朝向的一致性。
    // 因此无需 save/restore CULL_FACE。

    int total_floats = capped_n * 3;  // capped_n 个顶点 × 3 floats

    glUseProgram(prog);
    glUniformMatrix4fv(glGetUniformLocation(prog, "uMVP"),
                       1, GL_FALSE, mvp);
    glUniform4f(glGetUniformLocation(prog, "uColor"),
                cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);

    // 上传到专用 VBO
    glBindBuffer(GL_ARRAY_BUFFER, strip_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(total_floats * sizeof(float)),
                 cmd.vertices.data(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, capped_n);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void Primitives3DRenderer::DrawLine3D(const Line3DCommand& cmd,
                                      unsigned int stream_vbo,
                                      unsigned int prog,
                                      const float mvp[16]) {
    float verts[6] = {
        cmd.p1.x(), cmd.p1.y(), cmd.p1.z(),
        cmd.p2.x(), cmd.p2.y(), cmd.p2.z(),
    };

    glUseProgram(prog);
    glUniformMatrix4fv(glGetUniformLocation(prog, "uMVP"),
                       1, GL_FALSE, mvp);
    glUniform4f(glGetUniformLocation(prog, "uColor"),
                cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);

    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glDrawArrays(GL_LINES, 0, 2);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void Primitives3DRenderer::DrawText3D(const Text3DCommand& cmd,
                                      unsigned int stream_vbo,
                                      unsigned int prog,
                                      const float mvp[16], int fbo_w,
                                      int fbo_h) {
    (void)stream_vbo;
    (void)prog;
    (void)mvp;
    (void)fbo_w;
    (void)fbo_h;
    (void)cmd;
    LOG_FIRST_N(WARNING, 1) << "DrawText3D: not yet implemented, skipping";
}

}  // namespace jpov
