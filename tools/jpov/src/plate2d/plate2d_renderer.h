// JPOV Plate2D — SDF 支配的 2D 板材渲染
//
// 定位（Danis 2026-10-27 定）：sp_ui 的视觉基座。一块「板材」= 圆角矩形轮廓 +
// 边框带 + 材质填充，全部由**片元里的解析距离场**决定，不用 CPU 三角化。
//
// 为什么是距离场而不是几何：
//   - 轮廓在**像素空间**里解析求交，任意尺寸/圆角/DPR 都是 1px 解析抗锯齿，
//     而 CPU 三角化（DrawFillRect2D 每角 12 段 + 独立边框环）放大后可见多边形面、
//     边框内外环段数不匹配处宽窄跳变；
//   - 边框带、渐变消失带、材质平铺/翻书全部只是对同一个标量 d 做加减 ⇒ **零额外几何、
//     零额外 draw call、不会在交接处裂开**。
//
// 坐标：屏幕空间像素（原点左上角），与其它 2D 命令一致。距离场用 gl_FragCoord 现算
// （见 kFs 的 ScreenPos），所以 d 天然是「像素单位」，fwidth(d) 就是像素足迹。
//
// 混合：输出**预乘 alpha**，内部临时切 (GL_ONE, GL_ONE_MINUS_SRC_ALPHA)，
// 退出时恢复 2D 默认 (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)。
// 预乘的原因：填充与边框带是同一块片元的两个不相交区域（coverage 相减），
// 直通 alpha 叠加会在内沿把颜色衰减两次 ⇒ 一条暗线。
//
// 已知边界（本版有意不做，见 tools/jpov/known_issue.md §9）：
//   - 只有矩形/圆角矩形/胶囊（单一半径），没有任意形状、没有每角独立半径；
//   - 没有外发光/内阴影（要等 Plate 有第二个消费者再加）。

#ifndef JPOV_SRC_PLATE2D_PLATE2D_RENDERER_H_
#define JPOV_SRC_PLATE2D_PLATE2D_RENDERER_H_

#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/src/texture_manager.h"

namespace jpov {

class Plate2DRenderer {
public:
    Plate2DRenderer() = delete;

    // 绘制一块板材。
    //
    // stream_vbo: 调用方共享的 GL_ARRAY_BUFFER（容量 ≥ 6 顶点 × 2 float）
    // prog:       由 Renderer::PlateProg() 编译的 program
    // texture_mgr: 仅 cmd.texture_id != 0 时需要（用于解析 GL 纹理与尺寸）
    // fbo_w/h:    当前 FBO 像素尺寸（须与 glViewport 一致，距离场用它换算屏幕坐标）
    //
    // GL 状态前置要求（调用方负责）：FBO 已绑定、glViewport(0,0,fbo_w,fbo_h)、
    // glEnable(GL_BLEND)。
    // Pre-condition: cmd.size 各分量 > 0；radius ∈ [0, min(size)/2]；
    //                border_width ∈ [0, min(size)/2]；border_fade ∈ [0, border_width]；
    //                tiling 各分量 > 0；tiles_x/tiles_y >= 1；flipbook_fps >= 0；
    //                texture_id == 0 或已注册。
    static void DrawPlate2D(const Plate2DCommand& cmd,
                            unsigned int stream_vbo, unsigned int prog,
                            const TextureManager& texture_mgr,
                            int fbo_w, int fbo_h);

    // 顶点着色器：aPos(像素) → NDC。屏幕 y 向下，故 ndc.y 取反。
    static constexpr const char* kVs = R"glsl(
#version 330 core
layout(location = 0) in vec2 aPos;
uniform vec2 uFboSize;

void main() {
    vec2 ndc = (aPos / uFboSize) * 2.0 - 1.0;
    ndc.y = -ndc.y;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
)glsl";

    // 片元着色器：解析 SDF 圆角矩形 → 覆盖率 → 边框剖面 + 材质。
    //
    // 四段，全部围绕同一个标量 d：
    //   ① 形状：SdRoundBox（d<0 内部），Coverage() 给 1px 解析 AA；
    //   ② 边框带：Coverage(d) - Coverage(d + border_width)，圆角处宽度恒定；
    //   ③ 剖面：border_bump 把边框带当成一根环管，用假 3D 法线做明暗；
    //   ④ 填充：材质平铺 + 可选 flipbook，乘 fill_color。
    //
    // flipbook 约定与粒子一致（CPU 定帧、GPU 只换 UV）：时间由调用方给秒数，
    // 本 shader 只做 time*fps 求分数帧号 + 相邻帧混合。图集 tiles_x × tiles_y
    // 行主序、帧 0 在图片左上角（vrow 翻转）。cell 采样带半像素内缩 + fract 平铺
    // 在取 cell 之前（两处坑见 known_issue.md §8.1/§8.2）。
    static constexpr const char* kFs = R"glsl(
#version 330 core
out vec4 FragColor;

uniform vec2 uFboSize;        // FBO 像素尺寸（gl_FragCoord → 屏幕坐标用）
uniform vec2 uMin;            // 板材左上角（屏幕像素）
uniform vec2 uSize;           // 板材宽高（像素）
uniform float uRadius;        // 圆角半径（像素）

uniform vec4 uFillColor;      // 乘到材质上的 tint（无纹理时即本体色）
uniform vec4 uBorderColor;    // 边框基色
uniform float uBorderWidth;   // 边框带宽（像素，0 = 无边框）
uniform float uBorderBump;    // [0,1] 剖面曲率：0 = 平面带，1 = 圆管
uniform float uBorderFade;    // 渐变消失带宽（像素，从内沿向外衰减到 0）

uniform int uHasTex;          // 1 = 用材质
uniform sampler2D uTex;
uniform vec2 uTiling;         // 每边平铺次数
uniform vec2 uScroll;         // UV 平移（单位 = 一整张图）
uniform vec2 uTexSize;        // 纹理像素尺寸（cell 内缩要用）
uniform int uFlipbook;        // 1 = 按帧号裁 cell
uniform int uTilesX;
uniform int uTilesY;
uniform float uFrame;         // 分数帧号（CPU 由 time*fps 算出）

// 圆角矩形有向距离：<0 内部，>0 外部，单位=像素。
float SdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - r;
}

// 距离 → 覆盖率（1px 解析 AA）。
// px 用 fwidth(d) = 本像素的距离场足迹；d 在像素空间里算，所以这是精确的 1px 过渡。
float Coverage(float d, float px) {
    return clamp(0.5 - d / px, 0.0, 1.0);
}

// 屏幕坐标（原点左上、y 向下）。gl_FragCoord 原点左下、y 向上，故 y 需要翻。
vec2 ScreenPos() {
    return vec2(gl_FragCoord.x, uFboSize.y - gl_FragCoord.y);
}

// 材质采样：先 fract 平铺，再（可选）取 flipbook cell。
// 注意顺序 —— 先除 tiles 再 fract 会把平铺周期和 cell 周期搅在一起（known_issue §8.2）。
vec3 SampleMaterial(vec2 uv01) {
    vec2 uv = fract(uv01 * uTiling + uScroll);
    if (uFlipbook == 0) {
        return texture(uTex, uv).rgb;
    }
    float total = float(uTilesX * uTilesY);
    float f0 = floor(uFrame);
    vec2 cell_px = uTexSize / vec2(float(uTilesX), float(uTilesY));
    // 半像素内缩：否则双线性会在 cell 边界取到邻居帧的边缘 texel（known_issue §8.1）。
    vec2 uv_local = uv * (cell_px - 2.0) + 1.0;
    float col0 = mod(f0, float(uTilesX));
    float row0 = floor(f0 / float(uTilesX));
    // 行号翻转：图集帧 0 在图片左上角，而 GL 纹理 v 向上。
    vec2 origin0 = vec2(col0, float(uTilesY - 1) - row0) * cell_px;
    vec3 c0 = texture(uTex, (uv_local + origin0) / uTexSize).rgb;
    // 相邻帧混合（分数帧号），末帧回绕到首帧 ⇒ 循环连续。
    float f1 = mod(f0 + 1.0, total);
    float col1 = mod(f1, float(uTilesX));
    float row1 = floor(f1 / float(uTilesX));
    vec2 origin1 = vec2(col1, float(uTilesY - 1) - row1) * cell_px;
    vec3 c1 = texture(uTex, (uv_local + origin1) / uTexSize).rgb;
    return mix(c0, c1, uFrame - f0);
}

void main() {
    vec2 screen = ScreenPos();
    vec2 local = screen - (uMin + uSize * 0.5);   // 相对板材中心（y 向下）
    // 距离场对 y 符号不敏感（矩形对称），但剖面的「上」需要一致朝向：
    // 这里把 local.y 取反成 y 向上，供边框法线使用。
    vec2 p = vec2(local.x, -local.y);

    float d = SdRoundBox(p, uSize * 0.5, uRadius);
    float px = max(fwidth(d), 1e-4);

    // 整块覆盖率 / 内沿以内（= 填充区）覆盖率。两者相减即边框带，天然不重叠。
    float cov_outer = Coverage(d, px);
    if (cov_outer <= 0.0) {
        discard;
    }
    float cov_inner = Coverage(d + uBorderWidth, px);

    // ---- 填充（材质 × tint）----
    vec3 fill_rgb = uFillColor.rgb;
    float fill_a = uFillColor.a;
    if (uHasTex == 1) {
        vec2 uv01 = (screen - uMin) / uSize;      // 板材自身的 0..1 参数
        vec3 mat = SampleMaterial(uv01);
        fill_rgb *= mat;
    }

    // ---- 边框带（剖面明暗）----
    vec3 edge_rgb = uBorderColor.rgb;
    float edge_fade = 1.0;
    if (uBorderWidth > 0.0) {
        // t: 0 = 外沿，1 = 内沿
        float t = clamp(-d / uBorderWidth, 0.0, 1.0);
        // 环管剖面：法线从「朝外」经「朝上」回到「朝内」，bump=0 时退化为平顶。
        // 梯度归一化用 length 下限保护：距离场在平台区梯度接近 0，直接 normalize 会飞 NaN。
        vec2 grad = vec2(dFdx(d), dFdy(d));
        grad = grad / max(length(grad), 1e-6);
        float ang = (1.0 - 2.0 * t) * 1.5707963 * uBorderBump;
        vec3 n = normalize(vec3(grad * sin(ang), cos(ang)));
        vec3 L = normalize(vec3(-0.45, 0.70, 0.55));   // 左上偏前来光
        float lambert = max(dot(n, L), 0.0);
        float spec = pow(max(dot(reflect(-L, n), vec3(0.0, 0.0, 1.0)), 0.0), 28.0);
        // 0.45 底 + 0.85 漫反射 + 0.9 高光：bump=0 时只剩常量底（平面带）。
        edge_rgb = uBorderColor.rgb * (0.45 + 0.85 * lambert) + vec3(spec * 0.9);
        // 渐变消失带：靠近内沿 alpha 收到 0 —— 边框淡入填充，没有硬交接。
        if (uBorderFade > 0.0) {
            edge_fade = clamp((1.0 - t) * uBorderWidth / uBorderFade, 0.0, 1.0);
        }
    }

    // ---- 预乘合成：填充区 + 边框带（覆盖率相减，不重叠）----
    float w_fill = cov_inner * fill_a;
    float w_edge = (cov_outer - cov_inner) * uBorderColor.a * edge_fade;
    vec3 rgb = fill_rgb * w_fill + edge_rgb * w_edge;
    float a = clamp(w_fill + w_edge, 0.0, 1.0);
    if (a <= 0.0) {
        discard;
    }
    FragColor = vec4(rgb, a);   // 预乘：rgb 已乘过各自权重
}
)glsl";
};

}  // namespace jpov

#endif  // JPOV_SRC_PLATE2D_PLATE2D_RENDERER_H_
