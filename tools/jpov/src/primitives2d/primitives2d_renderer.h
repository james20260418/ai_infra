// JPOV Primitives2DRenderer — 2D 图元渲染模块
//
// 从 Renderer 中拆出的 2D 图元绘制方法（纯色 shader + 共享 stream VBO）。
// 所有绘制方法都是纯静态的，通过参数传入共享 GL 资源：
//   - stream_vbo: Renderer 持有的共享 GL_ARRAY_BUFFER（容量 ≥ 所需体积）
//   - prog:       2D 纯色 shader program（由 kVs + kFs 编译，位置坐标在 0）
//   - image_prog: 2D 纹理 shader program（由 kTexVs + kImageFs 编译，
//                 仅 DrawImage2D 需要）
//   - fbo_w/h:    当前 FBO 像素尺寸（NDC 变换用，需与 glViewport 一致）
//   - texture_mgr: 纹理管理器（仅 DrawImage2D 需要，用于按 texture_id 解析
//                  GL 纹理对象与尺寸）
//
// GL 状态前置要求（调用方 Renderer 负责）：
//   - FBO 已绑定（glBindFramebuffer）
//   - glViewport(0, 0, fbo_w, fbo_h)
//   - glEnable(GL_BLEND) + glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)
//
// 每个 Draw* 通过 glBufferData 将 CPU 顶点写入共享 VBO 后再 glDrawArrays，
// 不创建/持有自有 VAO。

#ifndef JPOV_PRIMITIVES2D_RENDERER_H_
#define JPOV_PRIMITIVES2D_RENDERER_H_

#include <vector>

#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/src/texture_manager.h"

namespace jpov {

class Text2DCommand;  // 未使用，避免不必要的 include

class Primitives2DRenderer {
public:
    Primitives2DRenderer() = delete;

    // 2D 图元绘制的顶点数上限（供调用方校验临检）
    static constexpr int kMaxPolylineEdges = 10000;

    // 绘制一条矩形边框（4 个顶点 GL_TRIANGLE_FAN）
    static void DrawRect2D(const Rect2DCommand& cmd,
                           unsigned int stream_vbo, unsigned int prog,
                           int fbo_w, int fbo_h);

    // 绘制一条折线（每段线段画成 quad + 连接处桥接三角形）
    static void DrawPolyline2D(const Polyline2DCommand& cmd,
                               unsigned int stream_vbo, unsigned int prog,
                               int fbo_w, int fbo_h);

    // 绘制一个实心圆（扇形 GL_TRIANGLE_FAN）
    static void DrawCircle2D(const Circle2DCommand& cmd,
                             unsigned int stream_vbo, unsigned int prog,
                             int fbo_w, int fbo_h);

    // 绘制一条 2D 三角带（GL_TRIANGLE_STRIP）
    static void DrawStrip2D(const Strip2DCommand& cmd,
                            unsigned int stream_vbo, unsigned int prog,
                            int fbo_w, int fbo_h);

    // 绘制圆角矩形（CPU 三角化 → GL_TRIANGLES）
    static void DrawRoundRect2D(const RoundRect2DCommand& cmd,
                                unsigned int stream_vbo, unsigned int prog,
                                int fbo_w, int fbo_h);

    // 绘制实心圆角矩形 + 可选边框环
    static void DrawFillRect2D(const FillRect2DCommand& cmd,
                               unsigned int stream_vbo, unsigned int prog,
                               int fbo_w, int fbo_h);

    // 绘制圆弧/扇形（CPU 三角化 → GL_TRIANGLES）
    static void DrawArc2D(const Arc2DCommand& cmd,
                          unsigned int stream_vbo, unsigned int prog,
                          int fbo_w, int fbo_h);

    // 绘制纹理矩形贴图（DrawImage2D，GL_TRIANGLES，interleaved xy+uv）
    static void DrawImage2D(const Image2DCommand& cmd,
                            unsigned int stream_vbo, unsigned int image_prog,
                            const TextureManager& texture_mgr,
                            int fbo_w, int fbo_h);

    // 圆角矩形填充三角化（RoundRect2D + FillRect2D 共享）。
    // radius<=0 时返回空（调用方用 GL_TRIANGLE_FAN 退化处理）。
    static std::vector<float> TriangulateRoundRectFill(
        const Vec2f& pos, const Vec2f& size, float radius);

private:
    static constexpr int kRoundCornerSegments = 12;
    static constexpr int kCircleFanSegments = 64;
    static constexpr int kArcFullCircleSegments = 48;
    static constexpr int kMaxStrip2DVertices = 3000;
};

}  // namespace jpov

#endif  // JPOV_PRIMITIVES2D_RENDERER_H_
