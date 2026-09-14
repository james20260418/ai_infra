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
    //
    // ⚠️ 重要坑（2026-08-17 影子方向排查实录）：下面这个数组是「行主序书写、
    // 列主序存储」的转置写法 —— 第一"列"（view[0..3]）装的是 side/upv/-fwd
    // 的 x 分量，而不是 side 向量本身。
    //
    // 教科书式 column-major 写法是：
    //     view[0]=side.x  view[4]=upv.x  view[8]=-fwd.x   // 第一列 = side 向量
    //     view[1]=side.y  view[5]=upv.y  view[9]=-fwd.y
    // 两者差一个转置！本代码库统一用下面这种（转置）约定，它配合
    //     glUniformMatrix4fv(column-major) + GLSL 的「mat4 * vec4」
    // 恰好得到正确结果。
    //
    // 若你新写/复制一个 lookAt（例如 shadow pass 的 DrawShadowPass 里），
    // 务必和这里保持同一套布局，否则 view 会整体转置 → 影子/投影的 x/y
    // 对调、depth 映射到错误轴（ndc.z 变成只由 x 决定）。
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
                                            float model[16],
                                            float scale) {
    // 归一化 up 与 front
    float u_len = std::sqrt(up.x()*up.x() + up.y()*up.y() + up.z()*up.z());
    float f_len = std::sqrt(front.x()*front.x() + front.y()*front.y() + front.z()*front.z());
    Vec3f upn = {up.x()/u_len, up.y()/u_len, up.z()/u_len};
    Vec3f frn = {front.x()/f_len, front.y()/f_len, front.z()/f_len};

    // right = normalize(cross(up, front))——保证模型右手系
    //
    // ⚠️ 命名坑（2026-08-17）：这里叫 right，但 MeshData::MakeBox 及 Danis 的
    // 接口定义里同一个向量 cross(up, front) 被称作「left」（左方向）。
    // 两者是同一个向量（数学表达式 identical，都是 up × front），只是命名
    // 相反。标准右手系 up=(0,1,0)、front=(0,0,1) 下，cross(up,front)=(1,0,0)
    // 即 +X。写/读涉及此方向的代码时别被 left/right 字面迷惑，认准表达式
    // cross(up, front) 即可。
    Vec3f right = {upn.y()*frn.z() - upn.z()*frn.y(),
                   upn.z()*frn.x() - upn.x()*frn.z(),
                   upn.x()*frn.y() - upn.y()*frn.x()};
    float r_len = std::sqrt(right.x()*right.x() + right.y()*right.y() + right.z()*right.z());
    right = {right.x()/r_len, right.y()/r_len, right.z()/r_len};

    CHECK_GT(scale, 0.0f) << "BuildModelMatrix: scale 必须 > 0，当前=" << scale;
    if (scale != 1.0f) {
        right = {right.x()*scale, right.y()*scale, right.z()*scale};
        upn   = {upn.x()*scale,   upn.y()*scale,   upn.z()*scale};
        frn   = {frn.x()*scale,   frn.y()*scale,   frn.z()*scale};
    }

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

// ==================== 3D 文本几何生成 ====================
//
// 正交基构建与 Object3DRenderer::BuildModelMatrix 同源（face 为权威方向，
// up 做 Gram-Schmidt；right = cross(up, front)）。这里把同一套逻辑用在
// 「文字平面」上，产出的是 5 floats/顶点（x,y,z,u,v）。
void Primitives3DRenderer::BuildText3DWorldVerts(
    const Text3DCommand& cmd,
    float font_height_px,
    std::vector<float>* glyph_verts,
    float* scale_out) {
    CHECK_NOTNULL(glyph_verts);
    CHECK_NOTNULL(scale_out);
    CHECK_GT(font_height_px, 0.0f);
    CHECK_GT(cmd.font_height_world, 0.0f);

    // ---- 1) 构建正交基（face 优先） ----
    // 注：拆成分量变量而非 Vec3f 成员访问，避免与 geom::Vec 的 .x()/.y()/.z()
    //     成员函数在表达式里歧义（n.x 会被解析成"未调用的成员函数"）。
    const float fnx = cmd.face_direction.x();
    const float fny = cmd.face_direction.y();
    const float fnz = cmd.face_direction.z();
    const float fl = std::sqrt(fnx*fnx + fny*fny + fnz*fnz);
    CHECK_GT(fl, 1e-8f) << "BuildText3DWorldVerts: face_direction 为零向量";
    // n = quad 法线（单位化）
    const float n0 = fnx / fl, n1 = fny / fl, n2 = fnz / fl;

    const float ux0 = cmd.up_direction.x();
    const float uy0 = cmd.up_direction.y();
    const float uz0 = cmd.up_direction.z();
    const float ul = std::sqrt(ux0*ux0 + uy0*uy0 + uz0*uz0);
    CHECK_GT(ul, 1e-8f) << "BuildText3DWorldVerts: up_direction 为零向量";

    // Gram-Schmidt：剔除 up 中平行于 n 的分量（face 权威）。
    const float dot_un = (ux0*n0 + uy0*n1 + uz0*n2) / ul;
    float u0 = ux0/ul - dot_un*n0;
    float u1 = uy0/ul - dot_un*n1;
    float u2 = uz0/ul - dot_un*n2;
    const float u_len = std::sqrt(u0*u0 + u1*u1 + u2*u2);
    CHECK_GT(u_len, 1e-8f)
        << "BuildText3DWorldVerts: up_direction 与 face_direction 共线，"
           "无法定义文字滚动（请给一个不平行于 face 的 up）";
    u0 /= u_len; u1 /= u_len; u2 /= u_len;

    // right = cross(u, n)：从正面看 u→右→n 构成右手系。
    //   与 Object3D 的 cross(up, front) 同一表达式（那边命名 left/right 不一，
    //   数学式完全相同，见 primitives3d_renderer.cc BuildModelMatrix 的命名坑注释）。
    const float r0 = u1*n2 - u2*n1;
    const float r1 = u2*n0 - u0*n2;
    const float r2 = u0*n1 - u1*n0;

    // ---- 2) 像素 → 米 的统一缩放 ----
    //   glyph 顶点来自 FontManager，其 y 以 font_height_px 为 em 高度排布；
    //   缩放到目标世界高度，scale = height_world / 像素高度。
    const float scale = cmd.font_height_world / font_height_px;

    // ---- 3) 逐顶点变换（每顶点 4 floats: x,y,u,v → 5 floats: x,y,z,u,v）----
    const size_t n_verts = glyph_verts->size() / 4;
    std::vector<float> out;
    out.resize(n_verts * 5);
    for (size_t i = 0; i < n_verts; ++i) {
        const float lx = (*glyph_verts)[i * 4 + 0];   // 文本平面局部 x（向右，像素）
        const float ly = (*glyph_verts)[i * 4 + 1];   // 文本平面局部 y（向下，像素）
        const float tu = (*glyph_verts)[i * 4 + 2];
        const float tv = (*glyph_verts)[i * 4 + 3];

        // 局部像素 → 世界米：
        //   world = anchor + r*(lx*scale) + u*(-ly*scale)
        // ly 取负：文本平面 y 向下（与 2D 屏幕同向），而 u 是「文字上」。
        const float sx = lx * scale;
        const float sy = -ly * scale;
        out[i * 5 + 0] = cmd.anchor.x() + r0*sx + u0*sy;
        out[i * 5 + 1] = cmd.anchor.y() + r1*sx + u1*sy;
        out[i * 5 + 2] = cmd.anchor.z() + r2*sx + u2*sy;
        out[i * 5 + 3] = tu;
        out[i * 5 + 4] = tv;
    }
    glyph_verts->swap(out);
    *scale_out = scale;
}

}  // namespace jpov
