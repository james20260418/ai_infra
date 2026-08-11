// JPOV Primitives2DRenderer 实现
//
// 2D 图元绘制（Rect/Circle/Polyline/Strip2D/RoundRect/FillRect/Arc/Image）。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/primitives2d/primitives2d_renderer.h"

#include <algorithm>
#include <cmath>
#include <vector>

// GL 头文件必须在 MinGW #define 之前
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
#include "third_party/gl_loader-mingw/gl_loader.h"
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#endif

#include <glog/logging.h>

namespace jpov {

std::vector<float> Primitives2DRenderer::TriangulateRoundRectFill(
    const Vec2f& pos, const Vec2f& size, float radius) {
    if (radius <= 0.0f) {
        return {};
    }

    float x0 = pos.x();
    float y0 = pos.y();
    float x1 = x0 + size.x();
    float y1 = y0 + size.y();
    float r = radius;

    // 圆角边界辅助点（内切边界坐标）
    // 左上角: (x0+r, y0+r) 右上角: (x1-r, y0+r)
    // 左下角: (x0+r, y1-r) 右下角: (x1-r, y1-r)
    float cx_inner = x0 + r;
    float cy_inner = y0 + r;
    float cx_outer = x1 - r;
    float cy_outer = y1 - r;

    int max_verts = 4 * (kRoundCornerSegments + 1) * 2  // 4 corners
                  + 4 * 6                                 // 4 edge rects (2 tris each)
                  + 6;                                    // center rect (2 tris)
    std::vector<float> verts;
    verts.reserve(max_verts);

    // ===== 中心矩形 (x0+r, y0+r) ~ (x1-r, y1-r) =====
    float center_x0 = cx_inner;
    float center_y0 = cy_inner;
    float center_x1 = cx_outer;
    float center_y1 = cy_outer;
    if (center_x1 > center_x0 && center_y1 > center_y0) {
        verts.push_back(center_x0); verts.push_back(center_y0);
        verts.push_back(center_x1); verts.push_back(center_y0);
        verts.push_back(center_x1); verts.push_back(center_y1);
        verts.push_back(center_x1); verts.push_back(center_y1);
        verts.push_back(center_x0); verts.push_back(center_y1);
        verts.push_back(center_x0); verts.push_back(center_y0);
    }

    // ===== 4 个边矩形（上、下、左、右） =====
    // 上边: (x0+r, y0) ~ (x1-r, y0+r)
    if (x1 - r > x0 + r) {
        verts.push_back(cx_inner);  verts.push_back(y0);
        verts.push_back(cx_outer);  verts.push_back(y0);
        verts.push_back(cx_outer);  verts.push_back(cy_inner);
        verts.push_back(cx_outer);  verts.push_back(cy_inner);
        verts.push_back(cx_inner);  verts.push_back(cy_inner);
        verts.push_back(cx_inner);  verts.push_back(y0);
    }
    // 下边: (x0+r, y1-r) ~ (x1-r, y1)
    if (x1 - r > x0 + r) {
        verts.push_back(cx_inner);  verts.push_back(cy_outer);
        verts.push_back(cx_outer);  verts.push_back(cy_outer);
        verts.push_back(cx_outer);  verts.push_back(y1);
        verts.push_back(cx_outer);  verts.push_back(y1);
        verts.push_back(cx_inner);  verts.push_back(y1);
        verts.push_back(cx_inner);  verts.push_back(cy_outer);
    }
    // 左边: (x0, y0+r) ~ (x0+r, y1-r)
    if (y1 - r > y0 + r) {
        verts.push_back(x0);       verts.push_back(cy_inner);
        verts.push_back(cx_inner); verts.push_back(cy_inner);
        verts.push_back(cx_inner); verts.push_back(cy_outer);
        verts.push_back(cx_inner); verts.push_back(cy_outer);
        verts.push_back(x0);       verts.push_back(cy_outer);
        verts.push_back(x0);       verts.push_back(cy_inner);
    }
    // 右边: (x1-r, y0+r) ~ (x1, y1-r)
    if (y1 - r > y0 + r) {
        verts.push_back(cx_outer); verts.push_back(cy_inner);
        verts.push_back(x1);       verts.push_back(cy_inner);
        verts.push_back(x1);       verts.push_back(cy_outer);
        verts.push_back(x1);       verts.push_back(cy_outer);
        verts.push_back(cx_outer); verts.push_back(cy_outer);
        verts.push_back(cx_outer); verts.push_back(cy_inner);
    }

    // ===== 4 个圆角区域（扇形三角形） =====
    // 左上角：圆心 (x0+r, y0+r)，从 180° 到 270°
    for (int seg = 0; seg < kRoundCornerSegments; ++seg) {
        double a0 = 3.14159265358979323846 * (180.0 + 90.0 * seg / kRoundCornerSegments) / 180.0;
        double a1 = 3.14159265358979323846 * (180.0 + 90.0 * (seg + 1) / kRoundCornerSegments) / 180.0;
        float px0 = cx_inner + r * std::cos(a0);
        float py0 = cy_inner + r * std::sin(a0);
        float px1 = cx_inner + r * std::cos(a1);
        float py1 = cy_inner + r * std::sin(a1);
        verts.push_back(cx_inner); verts.push_back(cy_inner);
        verts.push_back(px0);      verts.push_back(py0);
        verts.push_back(px1);      verts.push_back(py1);
    }
    // 右上角：圆心 (x1-r, y0+r)，从 270° 到 360°
    for (int seg = 0; seg < kRoundCornerSegments; ++seg) {
        double a0 = 3.14159265358979323846 * (270.0 + 90.0 * seg / kRoundCornerSegments) / 180.0;
        double a1 = 3.14159265358979323846 * (270.0 + 90.0 * (seg + 1) / kRoundCornerSegments) / 180.0;
        float px0 = cx_outer + r * std::cos(a0);
        float py0 = cy_inner + r * std::sin(a0);
        float px1 = cx_outer + r * std::cos(a1);
        float py1 = cy_inner + r * std::sin(a1);
        verts.push_back(cx_outer); verts.push_back(cy_inner);
        verts.push_back(px0);      verts.push_back(py0);
        verts.push_back(px1);      verts.push_back(py1);
    }
    // 右下角：圆心 (x1-r, y1-r)，从 0° 到 90°
    for (int seg = 0; seg < kRoundCornerSegments; ++seg) {
        double a0 = 3.14159265358979323846 * (90.0 * seg / kRoundCornerSegments) / 180.0;
        double a1 = 3.14159265358979323846 * (90.0 * (seg + 1) / kRoundCornerSegments) / 180.0;
        float px0 = cx_outer + r * std::cos(a0);
        float py0 = cy_outer + r * std::sin(a0);
        float px1 = cx_outer + r * std::cos(a1);
        float py1 = cy_outer + r * std::sin(a1);
        verts.push_back(cx_outer); verts.push_back(cy_outer);
        verts.push_back(px0);      verts.push_back(py0);
        verts.push_back(px1);      verts.push_back(py1);
    }
    // 左下角：圆心 (x0+r, y1-r)，从 90° 到 180°
    for (int seg = 0; seg < kRoundCornerSegments; ++seg) {
        double a0 = 3.14159265358979323846 * (90.0 + 90.0 * seg / kRoundCornerSegments) / 180.0;
        double a1 = 3.14159265358979323846 * (90.0 + 90.0 * (seg + 1) / kRoundCornerSegments) / 180.0;
        float px0 = cx_inner + r * std::cos(a0);
        float py0 = cy_outer + r * std::sin(a0);
        float px1 = cx_inner + r * std::cos(a1);
        float py1 = cy_outer + r * std::sin(a1);
        verts.push_back(cx_inner); verts.push_back(cy_outer);
        verts.push_back(px0);      verts.push_back(py0);
        verts.push_back(px1);      verts.push_back(py1);
    }

    return verts;
}


void Primitives2DRenderer::DrawStrip2D(const Strip2DCommand& cmd,
        unsigned int stream_vbo, unsigned int prog, int fbo_w, int fbo_h) {
    int n = static_cast<int>(cmd.vertices.size());
    if (n < 3) return;

    int capped_n = (n > kMaxStrip2DVertices) ? kMaxStrip2DVertices : n;
    int total_floats = capped_n * 2;  // Vec2f = 2 floats per vertex
    glUseProgram(prog);
    glUniform2f(glGetUniformLocation(prog, "uFboSize"),
                static_cast<float>(fbo_w), static_cast<float>(fbo_h));
    glUniform4f(glGetUniformLocation(prog, "uColor"),
                cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);

    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(total_floats * sizeof(float)),
                 cmd.vertices.data(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, capped_n);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

// ==================== 圆角矩形填充三角化（共享方法） ====================
//
// 将圆角矩形分成 9 个区域（4 个角 + 4 个边 + 1 个中心矩形），
// 每个 90° 圆角用 kRoundCornerSegments 个扇形三角形逼近。
// 返回的顶点数据用于 GL_TRIANGLES（非 fan）。
// radius=0 时退化返回空（调用方用 GL_TRIANGLE_FAN 处理）。
//
// 三角化拓扑（半径 r）：
//   圆角区域：以圆角内切矩形边界为锚点，计算圆弧上的点
//   边/中心区域：直接用矩形对三角形

void Primitives2DRenderer::DrawRoundRect2D(const RoundRect2DCommand& cmd,
        unsigned int stream_vbo, unsigned int prog, int fbo_w, int fbo_h) {
    // Render a round-topped rectangle using shared CPU-side triangulation
    // via TriangulateRoundRectFill().

    if (cmd.radius <= 0.0f) {
        // Degenerate to plain rectangle via GL_TRIANGLE_FAN
        float x0 = cmd.pos.x();
        float y0 = cmd.pos.y();
        float x1 = x0 + cmd.size.x();
        float y1 = y0 + cmd.size.y();
        float verts[8] = {x0, y0, x1, y0, x1, y1, x0, y1};
        glUseProgram(prog);
        glUniform2f(glGetUniformLocation(prog, "uFboSize"),
                    static_cast<float>(fbo_w), static_cast<float>(fbo_h));
        glUniform4f(glGetUniformLocation(prog, "uColor"),
                    cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);
        glBindBuffer(GL_ARRAY_BUFFER, stream_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        glDisableVertexAttribArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        return;
    }

    std::vector<float> verts = TriangulateRoundRectFill(
        cmd.pos, cmd.size, cmd.radius);

    // Render
    int total_verts = static_cast<int>(verts.size()) / 2;
    glUseProgram(prog);
    glUniform2f(glGetUniformLocation(prog, "uFboSize"),
                static_cast<float>(fbo_w), static_cast<float>(fbo_h));
    glUniform4f(glGetUniformLocation(prog, "uColor"),
                cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);

    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                 verts.data(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glDrawArrays(GL_TRIANGLES, 0, total_verts);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}


void Primitives2DRenderer::DrawFillRect2D(const FillRect2DCommand& cmd,
        unsigned int stream_vbo, unsigned int prog, int fbo_w, int fbo_h) {
    // FillRect2D 的实现策略：
    // 1. 填充部分：直接复用 TriangulateRoundRectFill() 三角化逻辑（用 fill_color）
    // 2. 边框部分：将圆角矩形边框环三角化为 GL_TRIANGLES（用 border_color）
    //
    // 边框环的几何定义：
    //   外圆角矩形 = (pos, size, radius)
    //   内圆角矩形 = (pos + border_width, size - 2*border_width, inner_radius)
    //   其中 inner_radius = max(0, radius - border_width)
    //
    // 边框环三角化：
    //   每个角区域：内外圆弧之间的扇形环
    //   每条边区域：内外矩形边之间的矩形带

    static constexpr int kMaxBorderVerts = 4 * kRoundCornerSegments * 6  // 4 corner rings, 2 tris/seg
                                          + 4 * 6;                      // 4 edge strips

    float x0 = cmd.pos.x();
    float y0 = cmd.pos.y();
    float x1 = x0 + cmd.size.x();
    float y1 = y0 + cmd.size.y();
    float r = cmd.radius;
    float bw = cmd.border_width;

    // ===== 第一步：填充部分 =====
    // 复用 TriangulateRoundRectFill() 产生三角形顶点
    glUseProgram(prog);
    glUniform2f(glGetUniformLocation(prog, "uFboSize"),
                static_cast<float>(fbo_w), static_cast<float>(fbo_h));
    glUniform4f(glGetUniformLocation(prog, "uColor"),
                cmd.fill_color.r, cmd.fill_color.g,
                cmd.fill_color.b, cmd.fill_color.a);

    if (r <= 0.0f) {
        float verts[8] = {x0, y0, x1, y0, x1, y1, x0, y1};
        glBindBuffer(GL_ARRAY_BUFFER, stream_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        glDisableVertexAttribArray(0);
    } else {
        std::vector<float> fill_verts = TriangulateRoundRectFill(
            cmd.pos, cmd.size, cmd.radius);
        int total_verts = static_cast<int>(fill_verts.size()) / 2;
        glBindBuffer(GL_ARRAY_BUFFER, stream_vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(fill_verts.size() * sizeof(float)),
                     fill_verts.data(), GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
        glDrawArrays(GL_TRIANGLES, 0, total_verts);
        glDisableVertexAttribArray(0);
    }

    // ===== 第二步：边框环三角化 =====
    if (bw <= 0.0f) {
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        return;
    }

    float in_bw = std::min(bw, std::min(cmd.size.x(), cmd.size.y()) * 0.5f);
    float inner_r = std::max(0.0f, r - in_bw);

    float ix0 = x0 + in_bw;
    float iy0 = y0 + in_bw;
    float ix1 = x1 - in_bw;
    float iy1 = y1 - in_bw;

    // 外圆角边界辅助点
    float cx_inner = x0 + r;
    float cy_inner = y0 + r;
    float cx_outer = x1 - r;
    float cy_outer = y1 - r;

    // 内圆角边界辅助点
    float icx_inner = ix0 + inner_r;
    float icy_inner = iy0 + inner_r;
    float icx_outer = ix1 - inner_r;
    float icy_outer = iy1 - inner_r;

    std::vector<float> border_verts;
    border_verts.reserve(kMaxBorderVerts);

    // ===== 边框边区域（矩形带） =====
    // 上边：外 (x0+r, y0) ~ (x1-r, y0) vs 内 (ix0+inner_r, iy0) ~ (ix1-inner_r, iy0)
    float outer_top_left = x0 + r;
    float outer_top_right = x1 - r;
    float inner_top_left = ix0 + inner_r;
    float inner_top_right = ix1 - inner_r;
    if (outer_top_right > outer_top_left && inner_top_right > inner_top_left) {
        border_verts.push_back(outer_top_left);  border_verts.push_back(y0);
        border_verts.push_back(outer_top_right); border_verts.push_back(y0);
        border_verts.push_back(inner_top_right); border_verts.push_back(iy0);
        border_verts.push_back(inner_top_right); border_verts.push_back(iy0);
        border_verts.push_back(inner_top_left);  border_verts.push_back(iy0);
        border_verts.push_back(outer_top_left);  border_verts.push_back(y0);
    }
    // 下边
    float outer_bot_left = x0 + r;
    float outer_bot_right = x1 - r;
    float inner_bot_left = ix0 + inner_r;
    float inner_bot_right = ix1 - inner_r;
    if (outer_bot_right > outer_bot_left && inner_bot_right > inner_bot_left) {
        border_verts.push_back(outer_bot_left);  border_verts.push_back(y1);
        border_verts.push_back(inner_bot_left);  border_verts.push_back(iy1);
        border_verts.push_back(inner_bot_right); border_verts.push_back(iy1);
        border_verts.push_back(inner_bot_right); border_verts.push_back(iy1);
        border_verts.push_back(outer_bot_right); border_verts.push_back(y1);
        border_verts.push_back(outer_bot_left);  border_verts.push_back(y1);
    }
    // 左边
    float outer_left_top = y0 + r;
    float outer_left_bot = y1 - r;
    float inner_left_top = iy0 + inner_r;
    float inner_left_bot = iy1 - inner_r;
    if (outer_left_bot > outer_left_top && inner_left_bot > inner_left_top) {
        border_verts.push_back(x0);       border_verts.push_back(outer_left_top);
        border_verts.push_back(ix0);      border_verts.push_back(inner_left_top);
        border_verts.push_back(ix0);      border_verts.push_back(inner_left_bot);
        border_verts.push_back(ix0);      border_verts.push_back(inner_left_bot);
        border_verts.push_back(x0);       border_verts.push_back(outer_left_bot);
        border_verts.push_back(x0);       border_verts.push_back(outer_left_top);
    }
    // 右边
    float outer_right_top = y0 + r;
    float outer_right_bot = y1 - r;
    float inner_right_top = iy0 + inner_r;
    float inner_right_bot = iy1 - inner_r;
    if (outer_right_bot > outer_right_top && inner_right_bot > inner_right_top) {
        border_verts.push_back(x1);       border_verts.push_back(outer_right_top);
        border_verts.push_back(x1);       border_verts.push_back(outer_right_bot);
        border_verts.push_back(ix1);      border_verts.push_back(inner_right_bot);
        border_verts.push_back(ix1);      border_verts.push_back(inner_right_bot);
        border_verts.push_back(ix1);      border_verts.push_back(inner_right_top);
        border_verts.push_back(x1);       border_verts.push_back(outer_right_top);
    }

    // ===== 边框圆角区域（扇形环） =====
    auto add_ring_segment = [&](float ocx, float ocy, float icx, float icy,
                                double start_deg, double end_deg) {
        double a0 = 3.14159265358979323846 * start_deg / 180.0;
        double a1 = 3.14159265358979323846 * end_deg / 180.0;
        float ox0 = ocx + r * std::cos(a0);
        float oy0 = ocy + r * std::sin(a0);
        float ox1 = ocx + r * std::cos(a1);
        float oy1 = ocy + r * std::sin(a1);
        float ix0_ = icx + inner_r * std::cos(a0);
        float iy0_ = icy + inner_r * std::sin(a0);
        float ix1_ = icx + inner_r * std::cos(a1);
        float iy1_ = icy + inner_r * std::sin(a1);
        border_verts.push_back(ox0);  border_verts.push_back(oy0);
        border_verts.push_back(ox1);  border_verts.push_back(oy1);
        border_verts.push_back(ix0_); border_verts.push_back(iy0_);
        border_verts.push_back(ix0_); border_verts.push_back(iy0_);
        border_verts.push_back(ox1);  border_verts.push_back(oy1);
        border_verts.push_back(ix1_); border_verts.push_back(iy1_);
    };

    if (inner_r > 0.0f) {
        // 左上角
        for (int seg = 0; seg < kRoundCornerSegments; ++seg) {
            add_ring_segment(
                cx_inner, cy_inner, icx_inner, icy_inner,
                180.0 + 90.0 * seg / kRoundCornerSegments,
                180.0 + 90.0 * (seg + 1) / kRoundCornerSegments);
        }
        // 右上角
        for (int seg = 0; seg < kRoundCornerSegments; ++seg) {
            add_ring_segment(
                cx_outer, cy_inner, icx_outer, icy_inner,
                270.0 + 90.0 * seg / kRoundCornerSegments,
                270.0 + 90.0 * (seg + 1) / kRoundCornerSegments);
        }
        // 右下角
        for (int seg = 0; seg < kRoundCornerSegments; ++seg) {
            add_ring_segment(
                cx_outer, cy_outer, icx_outer, icy_outer,
                90.0 * seg / kRoundCornerSegments,
                90.0 * (seg + 1) / kRoundCornerSegments);
        }
        // 左下角
        for (int seg = 0; seg < kRoundCornerSegments; ++seg) {
            add_ring_segment(
                cx_inner, cy_outer, icx_inner, icy_outer,
                90.0 + 90.0 * seg / kRoundCornerSegments,
                90.0 + 90.0 * (seg + 1) / kRoundCornerSegments);
        }
    }

    // Render border
    if (!border_verts.empty()) {
        int total_border = static_cast<int>(border_verts.size()) / 2;
        glUseProgram(prog);
        glUniform2f(glGetUniformLocation(prog, "uFboSize"),
                    static_cast<float>(fbo_w), static_cast<float>(fbo_h));
        glUniform4f(glGetUniformLocation(prog, "uColor"),
                    cmd.border_color.r, cmd.border_color.g,
                    cmd.border_color.b, cmd.border_color.a);
        glBindBuffer(GL_ARRAY_BUFFER, stream_vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(border_verts.size() * sizeof(float)),
                     border_verts.data(), GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
        glDrawArrays(GL_TRIANGLES, 0, total_border);
        glDisableVertexAttribArray(0);
    }

    glBindBuffer(GL_ARRAY_BUFFER, 0);
}


void Primitives2DRenderer::DrawArc2D(const Arc2DCommand& cmd,
        unsigned int stream_vbo, unsigned int prog, int fbo_w, int fbo_h) {
    // CPU 端三角化圆弧/扇形：
    // 扇形近似：圆心 + 圆弧上 N 个扇形三角形（GL_TRIANGLES）。
    // 跨度角度绝对值 >= 360 时绘制完整圆形。
    // 角度为负时绘制顺时针方向。

    static constexpr int kArcSegs = kArcFullCircleSegments;  // 完整圆的三角形数

    // 计算实际跨度（归一化到 360 度内，支持多圈）
    float abs_span = std::fabs(cmd.span_angle);
    if (abs_span < 1e-6f) return;  // 零跨度，不绘制

    // 如果是完整圆或超过 360 度，绘制完整圆
    if (abs_span >= 360.0f - 1e-6f) {
        // 完整圆：圆形 + N 个三角形
        int tri_count = kArcSegs;
        std::vector<float> verts;
        verts.reserve(static_cast<size_t>(tri_count) * 3 * 2);

        float cx = cmd.center.x();
        float cy = cmd.center.y();
        float r = cmd.radius;

        double start_rad = 0.0;
        double step = 2.0 * 3.14159265358979323846 / kArcSegs;
        for (int i = 0; i < kArcSegs; ++i) {
            double a0 = start_rad + i * step;
            double a1 = start_rad + (i + 1) * step;
            float px0 = cx + r * std::cos(a0);
            float py0 = cy + r * std::sin(a0);
            float px1 = cx + r * std::cos(a1);
            float py1 = cy + r * std::sin(a1);
            verts.push_back(cx);  verts.push_back(cy);
            verts.push_back(px0); verts.push_back(py0);
            verts.push_back(px1); verts.push_back(py1);
        }

        int total_verts = static_cast<int>(verts.size()) / 2;
        glUseProgram(prog);
        glUniform2f(glGetUniformLocation(prog, "uFboSize"),
                    static_cast<float>(fbo_w), static_cast<float>(fbo_h));
        glUniform4f(glGetUniformLocation(prog, "uColor"),
                    cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);
        glBindBuffer(GL_ARRAY_BUFFER, stream_vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                     verts.data(), GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
        glDrawArrays(GL_TRIANGLES, 0, total_verts);
        glDisableVertexAttribArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        return;
    }

    // 非完整圆：扇形近似
    // 三角形数 = ceil(abs_span / 360 * kArcSegs)，至少 3
    float ratio = abs_span / 360.0f;
    int tri_count = std::max(3, static_cast<int>(kArcSegs * ratio + 0.5f));

    float cx = cmd.center.x();
    float cy = cmd.center.y();
    float r = cmd.radius;

    double start_rad = 3.14159265358979323846 * cmd.start_angle / 180.0;
    double span_rad = 3.14159265358979323846 * cmd.span_angle / 180.0;
    double step = span_rad / tri_count;

    std::vector<float> verts;
    verts.reserve(static_cast<size_t>(tri_count) * 3 * 2);

    for (int i = 0; i < tri_count; ++i) {
        double a0 = start_rad + i * step;
        double a1 = start_rad + (i + 1) * step;
        float px0 = cx + r * std::cos(a0);
        float py0 = cy + r * std::sin(a0);
        float px1 = cx + r * std::cos(a1);
        float py1 = cy + r * std::sin(a1);
        verts.push_back(cx);  verts.push_back(cy);
        verts.push_back(px0); verts.push_back(py0);
        verts.push_back(px1); verts.push_back(py1);
    }

    int total_verts = static_cast<int>(verts.size()) / 2;
    glUseProgram(prog);
    glUniform2f(glGetUniformLocation(prog, "uFboSize"),
                static_cast<float>(fbo_w), static_cast<float>(fbo_h));
    glUniform4f(glGetUniformLocation(prog, "uColor"),
                cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);
    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                 verts.data(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glDrawArrays(GL_TRIANGLES, 0, total_verts);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}


// 上传全部点光源数据到 lighting shader 的 flat uniform 数组。
//
// 每帧在 BuildTileLightIndices 之前调用一次（非逐 object 热路径，可接受
// 每光源 3 次 snprintf + glGetUniformLocation）。ShaderManager 会缓存
// uniform location，避免重复字符串查找。
//
// 只上传前 kMaxTotalLights_ 个光源；超限提示由 BuildTileLightIndices 负责
//（这是唯一 warning 触发点）。
//
// Pre-condition: MeshLighting3DProg() 已注册（首次调用会触发编译）。

// CPU 端 tile culling：
//   遍历 cmds.point_lights（用户已按优先级排好序），把每个光源投影到
//   NDC → 屏幕空间覆盖矩形 → 转换成 tile 坐标范围 → 向每个覆盖 tile 写入
//   light index（先到先得，每 tile 最多 kMaxLightsPerTile_ 个，后到的丢弃）。
//
// 输出到 tile_index_tex_（GL_RGBA8），fragment shader 按 gl_FragCoord 查表。
//
// 覆盖是保守近似：投影光源球的 6 个轴向边界点取屏幕 x/y 的 min/max，保证
// 装下球体；光源 radius（线性衰减边界）跨越相机时投影会失真，此时让该光源
// 覆盖整个屏幕，确保不漏光（宁多勿少）。这是近似而非精确锥体裁剪。

void Primitives2DRenderer::DrawPolyline2D(const Polyline2DCommand& cmd,
        unsigned int stream_vbo, unsigned int prog, int fbo_w, int fbo_h) {
    // Pre-condition:
    //   - vertices 至少 2 个点
    //   - edge_count (vertices.size()-1) ≤ Primitives2DRenderer::kMaxPolylineEdges
    //   - line_width > 0（像素单位）
    int n = static_cast<int>(cmd.vertices.size());
    CHECK_GE(n, 2);
    int edge_count = n - 1;
    CHECK_LE(edge_count, Primitives2DRenderer::kMaxPolylineEdges);
    CHECK_GT(cmd.line_width, 0.0f);

    // 每个 quad 6 顶点 + 每个 bridge 6 顶点（2 三角形）
    // 顶点格式：x, y, x, y, ...
    int total_verts = edge_count * 6 + (edge_count - 1) * 6;
    std::vector<float> verts;
    verts.reserve(static_cast<size_t>(total_verts) * 2);

    float half_w = cmd.line_width * 0.5f;

    for (int i = 0; i < edge_count; ++i) {
        const Vec2f& p0 = cmd.vertices[i];
        const Vec2f& p1 = cmd.vertices[i + 1];

        // 边向量
        Vec2f dir = p1 - p0;
        float len = std::sqrt(dir.x() * dir.x() + dir.y() * dir.y());

        // 垂直方向（归一化），len 过短时水平偏移
        Vec2f perp;
        static constexpr float kEpsilon = 1e-6f;
        if (len < kEpsilon) {
            perp = {1.0f, 0.0f};
        } else {
            perp = {-dir.y() / len, dir.x() / len};
        }

        // 四个角点
        Vec2f n0 = p0 + perp * half_w;
        Vec2f n1 = p0 - perp * half_w;
        Vec2f n2 = p1 + perp * half_w;
        Vec2f n3 = p1 - perp * half_w;

        // 两个三角形：n0-n1-n2, n2-n1-n3 (CW)
        // T1
        verts.push_back(n0.x()); verts.push_back(n0.y());
        verts.push_back(n1.x()); verts.push_back(n1.y());
        verts.push_back(n2.x()); verts.push_back(n2.y());
        // T2
        verts.push_back(n2.x()); verts.push_back(n2.y());
        verts.push_back(n1.x()); verts.push_back(n1.y());
        verts.push_back(n3.x()); verts.push_back(n3.y());
        // 每个连接处在 V 形间隙外侧补一个三角形
        if (i + 1 < edge_count) {
            const Vec2f& p2 = cmd.vertices[i + 2];
            Vec2f dn = p2 - p1;
            float ln = std::sqrt(dn.x()*dn.x()+dn.y()*dn.y());
            Vec2f perp_n;
            if (ln < kEpsilon) { perp_n = {1.0f, 0.0f}; }
            else { perp_n = {-dn.y()/ln, dn.x()/ln}; }

            // 用顶点和两段矩形外侧角点构成填充三角形
            // 内侧三角形 (p1, n3, n3_next) 和 (p1, n2_next, n2) 填充间隙
            verts.push_back(p1.x()); verts.push_back(p1.y());
            verts.push_back(n3.x()); verts.push_back(n3.y());
            verts.push_back((p1 - perp_n * half_w).x());
            verts.push_back((p1 - perp_n * half_w).y());

            verts.push_back(p1.x()); verts.push_back(p1.y());
            verts.push_back((p1 + perp_n * half_w).x());
            verts.push_back((p1 + perp_n * half_w).y());
            verts.push_back(n2.x()); verts.push_back(n2.y());
        }
    }

    CHECK_LE(total_verts, 120000);
    glUseProgram(prog);
    glUniform2f(glGetUniformLocation(prog, "uFboSize"),
                static_cast<float>(fbo_w), static_cast<float>(fbo_h));
    glUniform4f(glGetUniformLocation(prog, "uColor"),
                cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);

    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                 verts.data(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glDrawArrays(GL_TRIANGLES, 0, total_verts);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}


void Primitives2DRenderer::DrawRect2D(const Rect2DCommand& cmd,
        unsigned int stream_vbo, unsigned int prog, int fbo_w, int fbo_h) {
    float verts[8];
    float x0 = cmd.pos.x();
    float y0 = cmd.pos.y();
    float x1 = x0 + cmd.size.x();
    float y1 = y0 + cmd.size.y();
    verts[0] = x0;
    verts[1] = y0;
    verts[2] = x1;
    verts[3] = y0;
    verts[4] = x1;
    verts[5] = y1;
    verts[6] = x0;
    verts[7] = y1;
    glUseProgram(prog);
    // uFboSize = NDC 变换参照。必须用 FBO 尺寸（渲染分辨率），
    // 使 NDC 坐标空间与 glViewport 一致，避免 rect 偏移。
    glUniform2f(glGetUniformLocation(prog, "uFboSize"),
                static_cast<float>(fbo_w), static_cast<float>(fbo_h));
    glUniform4f(glGetUniformLocation(prog, "uColor"),
                cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);

    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}


void Primitives2DRenderer::DrawCircle2D(const Circle2DCommand& cmd,
        unsigned int stream_vbo, unsigned int prog, int fbo_w, int fbo_h) {
    static constexpr int kSegments = kCircleFanSegments;
    float verts[(kSegments + 2) * 2];  // fan center + kSegments perimeter points
    float cx = cmd.center.x();
    float cy = cmd.center.y();
    float r = cmd.radius;

    // Center of fan
    verts[0] = cx;
    verts[1] = cy;

    constexpr double kPi = 3.14159265358979323846;
    for (int i = 0; i <= kSegments; ++i) {
        double angle = 2.0 * kPi * static_cast<double>(i) /
                       static_cast<double>(kSegments);
        verts[(i + 1) * 2 + 0] = cx + r * static_cast<float>(cos(angle));
        verts[(i + 1) * 2 + 1] = cy + r * static_cast<float>(sin(angle));
    }
    glUseProgram(prog);
    glUniform2f(glGetUniformLocation(prog, "uFboSize"),
                static_cast<float>(fbo_w), static_cast<float>(fbo_h));
    glUniform4f(glGetUniformLocation(prog, "uColor"),
                cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);

    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glDrawArrays(GL_TRIANGLE_FAN, 0, kSegments + 2);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}


void Primitives2DRenderer::DrawImage2D(const Image2DCommand& cmd,
        unsigned int stream_vbo, unsigned int image_prog,
        const TextureManager& texture_mgr, int fbo_w, int fbo_h) {
    // 查找纹理
    int tex_w = 0;
    int tex_h = 0;
    bool found = texture_mgr.GetSize(cmd.texture_id, &tex_w, &tex_h);
    CHECK(found) << "DrawImage2D: texture_id=" << cmd.texture_id
                 << " not registered";
    CHECK_GT(tex_w, 0);
    CHECK_GT(tex_h, 0);

    uint32_t gl_tex = texture_mgr.GetGLTexture(cmd.texture_id);
    CHECK_NE(gl_tex, 0u) << "DrawImage2D: texture_id=" << cmd.texture_id
                          << " has no GL texture";

    // 构建矩形面片：pos → pos+size，UV (0,0)→(1,1)
    // 顶点格式：[x, y, u, v] interleaved，6 顶点（2 三角形）
    float x0 = cmd.pos.x();
    float y0 = cmd.pos.y();
    float x1 = x0 + cmd.size.x();
    float y1 = y0 + cmd.size.y();

    // T1: (x0,y0)-(x1,y0)-(x1,y1), T2: (x0,y0)-(x1,y1)-(x0,y1)
    // 屏幕坐标 y0=上 y1=下。shader 设 ndc.y = -ndc.y 使 y0→NDC 上方。
    // stb_image 像素第一行=图片顶部, glTexImage2D 把它放在 v=0 (GL 纹理底部),
    // 因此图片顶部对应 v=0, 图片底部对应 v=1。
    // 要把图片顶部映射到屏幕上方 (y0), 底部映射到屏幕下方 (y1):
    //   y0 → v=0, y1 → v=1
    float verts[24] = {
        x0, y0, 0.0f, 0.0f,  // T1: top-left
        x1, y0, 1.0f, 0.0f,  // T1: top-right
        x1, y1, 1.0f, 1.0f,  // T1: bottom-right
        x0, y0, 0.0f, 0.0f,  // T2: top-left
        x1, y1, 1.0f, 1.0f,  // T2: bottom-right
        x0, y1, 0.0f, 1.0f,  // T2: bottom-left
    };
    glUseProgram(image_prog);
    glUniform2f(glGetUniformLocation(image_prog, "uFboSize"),
                static_cast<float>(fbo_w), static_cast<float>(fbo_h));
    glUniform4f(glGetUniformLocation(image_prog, "uTint"),
                cmd.tint.r, cmd.tint.g, cmd.tint.b, cmd.tint.a);
    glUniform1i(glGetUniformLocation(image_prog, "uTexture"), 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gl_tex);

    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);

    constexpr int kStride = 4 * static_cast<int>(sizeof(float));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, kStride, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, kStride,
                          (void*)(2 * sizeof(float)));

    glDrawArrays(GL_TRIANGLES, 0, 6);

    GLenum draw_err = glGetError();
    if (draw_err != GL_NO_ERROR) {
        LOG_FIRST_N(WARNING, 1) << "GL error after DrawImage2D: " << draw_err;
    }

    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

}  // namespace jpov
