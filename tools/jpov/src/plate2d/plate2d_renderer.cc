// JPOV Plate2D — DrawPlate2D 实现（GL 侧）
//
// 一块板材 = 一个 quad（6 顶点、只带位置）+ 一条片元着色器路径。
// 形状/边框/材质全在片元里算，见 plate2d_renderer.h 的 kFs。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/plate2d/plate2d_renderer.h"

#include <algorithm>
#include <cmath>
#include <string>

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

// 本 pass 独占的纹理单元。2D pass 内部所有 program（DrawImage2D / 本 pass）一律从 0 起绑，
// 不参与 3D 主链路的 `src/texture_units.h` 分配表（该表适用范围见其顶部注释：仅 3D 主链路）。
// 板材要两层材质 ⇒ 占 0 / 1。
constexpr int kTexUnitPlateFill = 0;
constexpr int kTexUnitPlateBorder = 1;

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
    // 两层材质各自校验（图集格式约定见 interface/flipbook.h）。
    cmd.fill.flipbook.Validate();
    cmd.border.flipbook.Validate();
    CHECK_GT(cmd.fill.tiling.x(), 0.0f) << "Plate2D fill.tiling.x 必须 > 0";
    CHECK_GT(cmd.fill.tiling.y(), 0.0f) << "Plate2D fill.tiling.y 必须 > 0";
    CHECK_GT(cmd.border.tiling.x(), 0.0f) << "Plate2D border.tiling.x 必须 > 0";
    CHECK_GT(cmd.border.tiling.y(), 0.0f) << "Plate2D border.tiling.y 必须 > 0";
    CHECK(std::isfinite(cmd.time_seconds))
        << "Plate2D time_seconds 必须是有限值";

    // 两层纹理各自解析（texture_id == 0 = 纯色，不采样）。
    struct ResolvedLayer {
        bool has_tex = false;
        unsigned int gl_tex = 0;
        int tex_w = 0;
        int tex_h = 0;
        bool flipbook = false;
        float frame = 0.0f;
    };
    auto resolve = [&](const PlateLayer& layer) {
        ResolvedLayer out;
        out.has_tex = layer.texture_id != 0;
        if (out.has_tex) {
            const bool found =
                texture_mgr.GetSize(layer.texture_id, &out.tex_w, &out.tex_h);
            CHECK(found) << "DrawPlate2D: texture_id=" << layer.texture_id
                         << " 未注册";
            CHECK_GT(out.tex_w, 0);
            CHECK_GT(out.tex_h, 0);
            out.gl_tex = texture_mgr.GetGLTexture(layer.texture_id);
            CHECK_NE(out.gl_tex, 0u) << "DrawPlate2D: texture_id="
                                     << layer.texture_id << " 没有 GL 纹理";
        }
        // 帧号：时间由调用方给（秒），fps 由各层自己给 —— 所以两层可以不同速。
        out.flipbook = out.has_tex &&
                       ResolveFlipbookFrame(layer.flipbook, cmd.time_seconds,
                                            &out.frame);
        return out;
    };
    const ResolvedLayer fill = resolve(cmd.fill);
    const ResolvedLayer border = resolve(cmd.border);

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

    auto upload_layer = [&](const char* prefix, const ResolvedLayer& l,
                            const PlateLayer& layout) {
        const std::string p(prefix);
        glUniform1i(glGetUniformLocation(prog, (p + "HasTex").c_str()),
                    l.has_tex ? 1 : 0);
        glUniform1i(glGetUniformLocation(prog, (p + "Flipbook").c_str()),
                    l.flipbook ? 1 : 0);
        glUniform2f(glGetUniformLocation(prog, (p + "Tiles").c_str()),
                    static_cast<float>(layout.flipbook.tiles_x),
                    static_cast<float>(layout.flipbook.tiles_y));
        glUniform2f(glGetUniformLocation(prog, (p + "Tiling").c_str()),
                    layout.tiling.x(), layout.tiling.y());
        glUniform2f(glGetUniformLocation(prog, (p + "Scroll").c_str()),
                    layout.scroll.x(), layout.scroll.y());
        glUniform2f(glGetUniformLocation(prog, (p + "TexSize").c_str()),
                    static_cast<float>(l.tex_w), static_cast<float>(l.tex_h));
        glUniform1f(glGetUniformLocation(prog, (p + "Frame").c_str()), l.frame);
    };
    upload_layer("uFill", fill, cmd.fill);
    upload_layer("uBorder", border, cmd.border);

    // 纹理单元号显式写死：0 = 填充层、1 = 边框层。2D pass 内部所有 program 一律从
    // 0 起绑（见 texture_units.h 顶部：那张表只适用于 3D 主链路）。
    glUniform1i(glGetUniformLocation(prog, "uFillTex"), kTexUnitPlateFill);
    glUniform1i(glGetUniformLocation(prog, "uBorderTex"), kTexUnitPlateBorder);
    glActiveTexture(GL_TEXTURE0 + kTexUnitPlateFill);
    glBindTexture(GL_TEXTURE_2D, fill.gl_tex);
    glActiveTexture(GL_TEXTURE0 + kTexUnitPlateBorder);
    glBindTexture(GL_TEXTURE_2D, border.gl_tex);
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
    glActiveTexture(GL_TEXTURE0 + kTexUnitPlateFill);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0 + kTexUnitPlateBorder);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(0);
}

}  // namespace jpov
