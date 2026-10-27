// JPOV Plate2D — DrawPlate2D 实现（GL 侧）
//
// 一块板材 = 一个 quad（6 顶点、只带位置）+ 一条片元着色器路径。
// 形状/边框/材质全在片元里算，见 plate2d_renderer.h 的 kFs。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/plate2d/plate2d_renderer.h"

#include <algorithm>

// GL 头文件必须在 MinGW #define 之前（同 primitives2d_renderer.cc）。
#include <GL/gl.h>
#ifndef _WIN32
#include <GL/glext.h>
#endif

#ifdef _WIN32
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
#include "third_party/gl_loader-mingw/gl_loader.h"
#endif

#include <glog/logging.h>

namespace jpov {

// 本 pass 独占的纹理单元。2D pass 内部所有 program（DrawImage2D / 本 pass）一律用 0 号，
// 不参与 3D 主链路的 `src/texture_units.h` 分配表（该表适用范围见其顶部注释：仅 3D 主链路）。
constexpr int kTexUnitPlate2D = 0;

void Plate2DRenderer::DrawPlate2D(const Plate2DCommand& cmd,
                                  unsigned int stream_vbo, unsigned int prog,
                                  const TextureManager& texture_mgr,
                                  int fbo_w, int fbo_h) {
    CHECK_GT(cmd.size.x(), 0.0f) << "Plate2D size.x 必须 > 0";
    CHECK_GT(cmd.size.y(), 0.0f) << "Plate2D size.y 必须 > 0";
    const float half_min = std::min(cmd.size.x(), cmd.size.y()) * 0.5f;
    CHECK_GE(cmd.radius, 0.0f) << "Plate2D radius 必须 >= 0";
    CHECK_LE(cmd.radius, half_min)
        << "Plate2D radius " << cmd.radius << " 超过短边一半 " << half_min;
    CHECK_GE(cmd.border_width, 0.0f) << "Plate2D border_width 必须 >= 0";
    CHECK_LE(cmd.border_width, half_min)
        << "Plate2D border_width " << cmd.border_width << " 超过短边一半 " << half_min;
    CHECK_GE(cmd.border_bump, 0.0f) << "Plate2D border_bump 必须 >= 0";
    CHECK_LE(cmd.border_bump, 1.0f) << "Plate2D border_bump 必须 <= 1";
    CHECK_GE(cmd.border_fade, 0.0f) << "Plate2D border_fade 必须 >= 0";
    CHECK_LE(cmd.border_fade, cmd.border_width)
        << "Plate2D border_fade " << cmd.border_fade << " 超过 border_width "
        << cmd.border_width;
    CHECK_GT(cmd.tiling.x(), 0.0f) << "Plate2D tiling.x 必须 > 0";
    CHECK_GT(cmd.tiling.y(), 0.0f) << "Plate2D tiling.y 必须 > 0";
    CHECK_GE(cmd.tiles_x, 1) << "Plate2D tiles_x 必须 >= 1";
    CHECK_GE(cmd.tiles_y, 1) << "Plate2D tiles_y 必须 >= 1";
    CHECK_GE(cmd.flipbook_fps, 0.0f) << "Plate2D flipbook_fps 必须 >= 0";

    // 纹理：仅 texture_id != 0 时解析（纯色板材不需要纹理）。
    int tex_w = 0;
    int tex_h = 0;
    unsigned int gl_tex = 0;
    const bool has_tex = cmd.texture_id != 0;
    if (has_tex) {
        const bool found = texture_mgr.GetSize(cmd.texture_id, &tex_w, &tex_h);
        CHECK(found) << "DrawPlate2D: texture_id=" << cmd.texture_id << " 未注册";
        CHECK_GT(tex_w, 0);
        CHECK_GT(tex_h, 0);
        gl_tex = texture_mgr.GetGLTexture(cmd.texture_id);
        CHECK_NE(gl_tex, 0u) << "DrawPlate2D: texture_id=" << cmd.texture_id
                             << " 没有 GL 纹理";
    }

    // 分数帧号：时间由调用方给（秒），帧率由调用方给；这里只做乘法。
    // flipbook_fps == 0 视为静态（不裁 UV），此时 uFlipbook=0。
    const bool flipbook = has_tex && cmd.flipbook_fps > 0.0f;
    float frame = 0.0f;
    if (flipbook) {
        const float total = static_cast<float>(cmd.tiles_x * cmd.tiles_y);
        const float f = cmd.time_seconds * cmd.flipbook_fps;
        // 归一化到 [0,total) 后取分数帧号；末帧回绕由着色器 mod 处理。
        frame = f - total * std::floor(f / total);
        if (frame >= total) {
            frame = total - 1.0f;
        }
    }

    // 覆盖整块板材的 quad（屏幕像素）。两个三角形，只带位置属性。
    const float x0 = cmd.pos.x();
    const float y0 = cmd.pos.y();
    const float x1 = x0 + cmd.size.x();
    const float y1 = y0 + cmd.size.y();
    const float verts[12] = {
        x0, y0,
        x1, y0,
        x1, y1,
        x0, y0,
        x1, y1,
        x0, y1,
    };

    glUseProgram(prog);
    glUniform2f(glGetUniformLocation(prog, "uFboSize"),
                static_cast<float>(fbo_w), static_cast<float>(fbo_h));
    glUniform2f(glGetUniformLocation(prog, "uMin"), x0, y0);
    glUniform2f(glGetUniformLocation(prog, "uSize"),
                cmd.size.x(), cmd.size.y());
    glUniform1f(glGetUniformLocation(prog, "uRadius"), cmd.radius);

    glUniform4f(glGetUniformLocation(prog, "uFillColor"),
                cmd.fill_color.r, cmd.fill_color.g,
                cmd.fill_color.b, cmd.fill_color.a);
    glUniform4f(glGetUniformLocation(prog, "uBorderColor"),
                cmd.border_color.r, cmd.border_color.g,
                cmd.border_color.b, cmd.border_color.a);
    glUniform1f(glGetUniformLocation(prog, "uBorderWidth"), cmd.border_width);
    glUniform1f(glGetUniformLocation(prog, "uBorderBump"), cmd.border_bump);
    glUniform1f(glGetUniformLocation(prog, "uBorderFade"), cmd.border_fade);

    glUniform1i(glGetUniformLocation(prog, "uHasTex"), has_tex ? 1 : 0);
    glUniform1i(glGetUniformLocation(prog, "uFlipbook"), flipbook ? 1 : 0);
    glUniform2f(glGetUniformLocation(prog, "uTiling"),
                cmd.tiling.x(), cmd.tiling.y());
    glUniform2f(glGetUniformLocation(prog, "uScroll"),
                cmd.scroll.x(), cmd.scroll.y());
    glUniform2f(glGetUniformLocation(prog, "uTexSize"),
                static_cast<float>(tex_w), static_cast<float>(tex_h));
    glUniform1i(glGetUniformLocation(prog, "uTilesX"), cmd.tiles_x);
    glUniform1i(glGetUniformLocation(prog, "uTilesY"), cmd.tiles_y);
    glUniform1f(glGetUniformLocation(prog, "uFrame"), frame);
    // 纹理单元号显式写死（strip 那轮的教训：sampler 单元号一旦靠下标推导，中间插入
    // 别的纹理就会整块静默无输出）。0 号单元在本 pass 里没有别人用。
    glUniform1i(glGetUniformLocation(prog, "uTex"), kTexUnitPlate2D);
    glActiveTexture(GL_TEXTURE0 + kTexUnitPlate2D);
    glBindTexture(GL_TEXTURE_2D, gl_tex);
    glActiveTexture(GL_TEXTURE0);

    // 预乘 alpha 混合，退出时恢复 2D 默认（见 .h 顶部说明）。
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glActiveTexture(GL_TEXTURE0 + kTexUnitPlate2D);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(0);
}

}  // namespace jpov
