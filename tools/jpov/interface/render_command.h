// JPOV RenderCommand — 帧级渲染指令输出
//
// OneIteration::Step() 产出 RenderCommandList，描述本帧要绘制的所有内容。
// 渲染后端消费这些指令将其画到屏幕上。
//
// 设计原则：
// - 流式绘制：每帧从零构建，无跨帧资源（顶点/颜色/文本都是字面量）
// - 不同图元命令类型不同，每种类型独立存储，通过 order 队列声明绘制顺序
// - 指令描述"画什么"，不描述"怎么画"
//
// 坐标系统一约定：
//   - 2D：屏幕像素坐标，原点在窗口左上角（x→右，y→下）
//   - 3D：世界空间，右手系（x→右，y→上，z→后）
//   - 2D 文本：包围盒角点对齐（见 TextAlignment 枚举），位置含义取决于对齐方式

#ifndef JPOV_RENDER_COMMAND_H_
#define JPOV_RENDER_COMMAND_H_

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <utility>

// geom/math_util.h 使用 M_PI，需在首次 include <cmath> 前定义 _USE_MATH_DEFINES，
// 否则 MinGW 下 M_PI 未定义（_USE_MATH_DEFINES 生效太晚）。
#define _USE_MATH_DEFINES
#include <cmath>

#include <glog/logging.h>
#include "geom/common/vec.h"
#include "geom/math/piecewise_linear_function.h"

#include "tools/jpov/interface/camera.h"
#include "tools/jpov/interface/pbr_material.h"
// 骨架蒙皮(command 引用 SkinnedInstanceState / skeleton_id)。GL-free：仅类型,不含 GPU 细节。
#include "tools/jpov/interface/skeleton_types.h"

namespace jpov {

// 前置声明：glTF 加载结果（完整定义在 gltf_object.h，避免 include 循环）。
// DrawGltfObject 只接受按值 const 引用，无需完整类型。
struct GltfObject;

// ==================== 类型别名 ====================

// 复用 geom 库的向量类型
// Vec2f 用于屏幕空间（像素坐标），Vec3f 用于世界空间
using Vec2f = geom::Vec2<float>;
using Vec3f = geom::Vec3<float>;

// ==================== 字体别名常量 ====================

// 推荐字体别名常量（“CJK”/“Latin” 纯文本，非自动注册）。
// JPOV 不提供隐式默认字体：用户必须在 JPOV::Config::fonts 显式注册字体，
// 并可用这些常量作为 alias，然后在 DrawText / UiTheme::font_alias 中引用。
// 注意：stb_truetype 对 CFF (PostScript outline) 格式支持不稳定，
//       仅 TrueType outline (.ttf/.ttc) 字体可用。
inline constexpr const char* kFontBuiltinCJK   = "CJK";   // NotoSansCJK-Regular.ttc (TTC index=0)
inline constexpr const char* kFontBuiltinLatin = "Latin"; // DejaVuSans.ttf

// ==================== 颜色 ====================

// ==================== 渲染指令类型 ====================

// 渲染命令类型的枚举。
// 每种类型对应一种图元的绘制，拥有独立的结构体。
enum class DrawCommandType : uint8_t {
    kPolyline2D,        // 2D 折线（屏幕空间，像素坐标）
    kRect2D,            // 2D 实心矩形（屏幕空间）
    kCircle2D,          // 2D 实心圆（屏幕空间）
    kText2D,            // 2D 文本（屏幕空间）
    kLine3D,            // 3D 线段（世界空间）
    kTriangle3D,        // 3D 三角形（世界空间）
    kStrip3D,           // 3D 条带（世界空间，多个三角形按条带化排列，VBO 加速）
    kText3D,            // 3D 文本（世界空间，面向摄像机）
    kStrip2D,           // 2D 条带（屏幕空间，像素坐标，
                        //      多个三角形按条带化排列，
                        //      直接画到主 FBO）
    kRoundRect2D,       // 2D 圆角矩形（屏幕空间，像素坐标，
                        //      带圆角半径参数，CPU 三角化）
    kFillRect2D,        // 2D 复合矩形（屏幕空间，像素坐标，
                        //      带填充色、边框颜色/宽度、圆角半径，
                        //      利用 RoundRect2D + Strip2D 组合实现）
    kArc2D,             // 2D 圆弧/扇形（屏幕空间，像素坐标，
                        //      圆心+半径+起始角度+跨度角度，
                        //      CPU 三角化后以三角形列表渲染）
    kImage2D,           // 2D 图片（屏幕空间，像素坐标，
                        //      GPU 纹理采样 + 矩形面片）
    kObject3D,          // 3D 静态模型（世界空间）
                        //      GPU mesh + 纹理 + 平移 center + 旋转 up/front
    kSkinnedMesh,       // 3D 骨架蒙皮模型（世界空间，instancing）
                        //      同 mesh+skeleton+clip 的一批实例 = 一次 instanced draw
                        //      见 SkinnedMeshCommand / DrawMeshWithSkeleton
};

// ==================== 各类绘制命令结构体 ====================

// 2D 折线（渲染分辨率空间，像素坐标，非闭合，方角端点）
// vertices: 折线的顶点序列（像素坐标，原点在渲染分辨率左上角）
// color: 整条线统一颜色
// line_width: 线宽（像素单位，>0）
// Pre-condition: vertices.size() >= 2
// Pre-condition: vertices.size() - 1 <= Renderer::kMaxPolylineEdges
//
// 坐标空间同 camera.fbo_3d_width_/height_
struct Polyline2DCommand {
    std::vector<Vec2f> vertices;
    Color color;
    float line_width;
};

// 2D 实心矩形（渲染分辨率空间，像素坐标）
//
// pos:  矩形左上角位置（像素坐标，原点在渲染分辨率左上角，x→右，y→下）
// size: 矩形的宽度和高度（像素单位，>0）
// color: 填充颜色（RGBA，分量范围 [0,1]，alpha < 1 时 blend）
//
// 坐标空间说明：
//   用户先通过 camera.fbo_3d_width_/height_ 声明渲染分辨率，
//   此后所有 DrawRect 的 pos/size 以该分辨率为空间。
//   渲染分辨率 ≠ 窗口尺寸——当两者不同时，最终输出会被拉伸贴合窗口。
//
//   例如 fbo_3d_width_=640, fbo_3d_height_=360：
//     矩形 (0,0, 320,180) 占据左上 1/4 区域
//     矩形 (160,90, 320,180) 居中
// Pre-condition: size.x > 0 && size.y > 0
struct Rect2DCommand {
    Vec2f pos;
    Vec2f size;
    Color color;
};

// 2D 实心圆（屏幕空间）
// center: 圆心
// radius: 半径（像素单位）
// color: 填充颜色
struct Circle2DCommand {
    Vec2f center;
    float radius;
    Color color;
};

// 文本对齐方式（2D 文本）
//
// 对齐点参考的是文本的包围盒边界，不是基线。
// - kTopLeft:     pos 为包围盒左上角（缺省值，保持向后兼容）
// - kTopRight:    pos 为包围盒右上角
// - kCenter:      pos 为包围盒水平垂直中心
// - kBottomLeft:  pos 为包围盒左下角
// - kBottomRight: pos 为包围盒右下角
// - kMidLeft:     pos 为包围盒左边中点
// - kMidRight:    pos 为包围盒右边中点
// - kMidTop:      pos 为包围盒上边中点
// - kMidBottom:   pos 为包围盒下边中点
enum class TextAlignment : uint8_t {
    kTopLeft = 0,
    kTopRight = 1,
    kCenter = 2,
    kBottomLeft = 3,
    kBottomRight = 4,
    kMidLeft = 5,
    kMidRight = 6,
    kMidTop = 7,
    kMidBottom = 8,
};

// 2D 文本（屏幕空间）
// text: 文本内容
// pos: 文本位置（像素单位，含义取决于 alignment）
// font_size: 字号（像素单位）
// color: 文本颜色
// alignment: 文本对齐方式
// font_alias: 字体别名（与 JPOV::Config::FontEntry::alias 对应）
//             空字符串（默认）使用第一个注册字体
struct Text2DCommand {
    std::string text;
    Vec2f pos;
    float font_size;
    Color color;
    TextAlignment alignment = TextAlignment::kTopLeft;
    std::string font_alias;
};

// 3D 线段（世界空间）
// p1, p2: 线段端点（世界坐标）
// color: 线段颜色
// width: 线段视觉厚度
//         软件渲染器简化为 1px（最小单位），
//         GL 后端可用 glLineWidth 处理。
//         不表示圆柱体或条带，仅在光栅化阶段影响像素覆盖。
struct Line3DCommand {
    Vec3f p1;
    Vec3f p2;
    Color color;
    float width;
};

// 3D 三角形（世界空间，实心，参与深度测试）
struct Triangle3DCommand {
    Vec3f p1;
    Vec3f p2;
    Vec3f p3;
    Color color;
};

// 3D 条带（世界空间，VBO 加速）
//
// 用顶点序列定义三维条带：vertices = [p0, p1, p2, p3, ...]
// 生成三角形：p0-p1-p2, p1-p2-p3, p2-p3-p4, ...
// 要求 vertices.size() >= 3，否则不绘制。
//
// 使用 GL VBO 存储顶点数据，每次绘制时更新 GPU 缓存中的顶点位置。
// VBO 在渲染器初始化时分配，跨帧共享（cache 语义）。
//
// 顶点缓存上限为 3000 个顶点（1000 个三角形）。
// 若 vertices.size() > 3000，条带被截断，仅前 3000 个顶点参与绘制。
// 此限制在注释中说明，调用方应避免超限以保证行为可预期。
struct Strip3DCommand {
    std::vector<Vec3f> vertices;
    Color color;
};

// 3D 文本（世界空间，面向摄像机）
//
// 实现方式：在 3D 空间建立矩形 mesh，渲染时应用文本纹理。
// 参与深度测试，被 3D 物体遮挡时自动隐藏。
//
// font_size: 世界空间中的文本大小（不是像素，是 3D 坐标单位）
// font_alias: 字体别名（与 JPOV::Config::FontEntry::alias 对应）
struct Text3DCommand {
    std::string text;
    Vec3f pos;
    float font_size;
    Color color;
    std::string font_alias;
};

// 2D 条带（屏幕空间，像素坐标）
//
// 用顶点序列定义二维三角形条带：vertices = [p0, p1, p2, p3, ...]
// 生成三角形：p0-p1-p2, p1-p2-p3, p2-p3-p4, ...
// 要求 vertices.size() >= 3，否则不绘制。
//
// 顶点坐标为像素坐标，原点在渲染分辨率左上角（x→右，y→下）。
// 绘制目标直接为主 FBO（无 3D MVP 变换）。
//
// 使用 GL_TRIANGLE_STRIP 渲染，stream VBO 每帧动态上传。
//
// 顶点缓存上限为 3000 个顶点（1000 个三角形）。
// 若 vertices.size() > 3000，条带被截断，仅前 3000 个顶点参与绘制。
struct Strip2DCommand {
    std::vector<Vec2f> vertices;
    Color color;
};

// 2D 圆角矩形（渲染分辨率空间，像素坐标）
//
// pos:    矩形左上角位置（像素坐标，原点在左上角）
// size:   矩形的宽度和高度（像素单位，>0）
// radius: 圆角半径（像素单位，>=0，不能超过短边一半）
// color:  填充颜色
//
// 坐标空间与 DrawRect 一致。
// CPU 端三角化后以三角形列表形式渲染。
// Pre-condition: size.x > 0 && size.y > 0
// Pre-condition: radius >= 0
// Pre-condition: radius <= min(size.x, size.y) / 2
struct RoundRect2DCommand {
    Vec2f pos;
    Vec2f size;
    float radius;
    Color color;
};

// 2D 复合矩形（渲染分辨率空间，像素坐标）
//
// 带填充色、边框颜色/宽度、圆角半径的复合矩形。
// 利用 RoundRect + Strip2D 组合实现：
//   - 填充部分：复写 RoundRect2DCommand 渲染逻辑
//   - 边框部分：将边框环三角化后通过 Strip2D 渲染
//
// Pre-condition: size.x > 0 && size.y > 0
// Pre-condition: radius >= 0
// Pre-condition: radius <= min(size.x, size.y) / 2
// Pre-condition: border_width >= 0
struct FillRect2DCommand {
    Vec2f pos;
    Vec2f size;
    Color fill_color;
    Color border_color;
    float border_width;
    float radius;
};

// 2D 圆弧/扇形（渲染分辨率空间，像素坐标）
//
// center:       圆心位置（像素坐标，原点在左上角）
// radius:       半径（像素单位，>0）
// start_angle:  起始角度（度，0=3点钟方向，逆时针为正）
// span_angle:   跨度角度（度，正数=逆时针，负数=顺时针）
// color:        填充颜色
//
// CPU 端三角化后以三角形列表（扇形）渲染。
// 跨度角度绝对值 >= 360 度时绘制完整圆形。
// Pre-condition: radius > 0
struct Arc2DCommand {
    Vec2f center;
    float radius;
    float start_angle;   // 起始角度（度）
    float span_angle;    // 跨度角度（度）
    Color color;
};

// 2D 图片（渲染分辨率空间，像素坐标）
//
// 通过 GPU 纹理采样将已注册的纹理绘制到指定矩形区域。
// 纹理由 JPOV::RegisterTexture 预先注册，获取 texture_id。
//
// texture_id:  纹理句柄（由 RegisterTexture 返回）
// pos:         绘制区域的左上角位置（像素坐标）
// size:        绘制区域的宽度和高度（像素单位，>0）
// tint:        色调乘数（默认白色=不染色，可用 alpha 控制透明度）
//
// 绘制时纹理会被 UV 映射到 [pos, pos+size] 矩形区域。
// 纹理原始宽高比与 size 不一致时会产生拉伸。
//
// Pre-condition: texture_id 已注册且未释放
// Pre-condition: size.x > 0 && size.y > 0
struct Image2DCommand {
    uint32_t texture_id;
    Vec2f pos;
    Vec2f size;
    Color tint;
};

// 点光源（世界空间）
//
// 光照计算采用 GGX PBR BRDF（Cook-Torrance：NDF + Fresnel + Geometry），
// 结果 = ambient * AO + diffuse + specular + emissive。
// 衰减为线性：强度随距离从 1.0（pos 处）衰减到 0.0（linear_radius 处），
// 超出 linear_radius 的光源对像素贡献为 0。
//
// 五个字段（构造时按此顺序 push_back 聚合初始化）：
//   { position, color, linear_radius, physical_radius, intensity }
// 例: { {3,0,0}, {1,1,1,1}, 6.0f, 0.5f, 1.0f }
//     = 位于 (3,0,0)，色调白、亮度 1.0（≈100W 灯泡）、有效距离 6 米、
//       光源球物理半径 0.5 米。
// 见 LIGHT_INTENSITY.md：color 只表达色调，亮度量级由 intensity 承载。
//
// Pre-condition: linear_radius > 0
struct PointLight {
    Vec3f position;
    Color color;              // 光源**色调/色温**（暖白/火光/霓虹等），区间 [0,1]
    float linear_radius;      // 线性衰减的最大有效距离（米）

    // 光源球体的物理半径（米）。
    //
    // Representative Point（Karis 2013, 'Real Shading in Unreal Engine 4')：
    // 默认 0 = 退化到纯点光源；非零时把光源当作一个有体积的球面，
    // 从光源球上选一个"代表点"（沿反射方向）来打 specular，
    // 让粗糙/金属表面也能从球的不同区域命中 specular lobe，
    // 避免金属面在纯点光源下"全黑"。这是光源变亮(面积大)时
    // 高光会柔化/扩大的原理。
    // 典型值：灯泡 ~0.02–0.05，方块大小 ~0.5。
    float physical_radius = 0.0f;

    // intensity：点光源亮度标量（乘到 color 上）。
    //   intensity=1.0 ≈ 一只 100 W 白炽灯泡（≈1600 流明）。
    //   点光源的几何/距离衰减由 linear_radius 独立处理，intensity 只锚定发光总量。
    float intensity = 1.0f;

    // 有效范围（供 culling 等使用）。
    // 当前线性衰减模式下直接返回 linear_radius；
    // 后续支持其他衰减函数时可通过此接口区分。
    float effective_range() const { return linear_radius; }
};

// 全局平行光（太阳 Directional Light）。
//
// 与点光源不同：无位置、无衰减、影响所有片元，因此不走 tile culling，
// 作为全局单一光源独立上传。平行光用正交 shadow map 产生影子
//（见 RenderCommandList.sun + Renderer 的 shadow pass）。
//
// direction 是光**传播方向**（从光源指向场景的单位向量，y-up 世界，
// 如正午太阳直射向下约 (0,-1,0)）。“朝上”（+y）的表面接收最强光照。
struct DirectionalLight {
    Vec3f direction;      // 光传播方向（从光源指向目标，建议归一化）
    Color color;          // 光**色温/色调**（太阳白光或暖色夕阳光），区间 [0,1]
                          // 见 LIGHT_INTENSITY.md：color 只表达色调，不承载亮度量级。
    float intensity = 1.0f;  // 亮度标量。intensity=1.0 ≈ 正午直射太阳 100,000 lux。
};

// 全局环境光（Ambient Light）—— 从四面八方均匀照亮物体的间接光。
//
// 与太阳平行光（DirectionalLight：有方向、有影子）和点光源（有位置、
// 有衰减）不同：环境光**无方向、无衰减、无影子**，是 PBR 结果里
// ambient * base_color * AO 那一项，让背阳面不至于全黑。
//
// 来源任意：天空（蓝天/夜空/晚霞）、岩浆湖（洞穴橙红）、雪地反光、
// 室内漫反射等。用户可直接手配；不配时使用默认值（后续可由 DaySkyCommand
// 依 sun_dir 自动推导一个兜底值）。
struct AmbientLight {
    // color：环境光**色调**（RGB，乘到材质 base_color 上）。
    //   - 中性灰白 (1,1,1) = 无色偏；
    //   - 天空日间可偏蓝，洞穴岩浆湖可偏橙红，夜空偏深蓝；
    //   - 见 LIGHT_INTENSITY.md：color 只表达色调，区间 [0,1]，不承载亮度量级。
    Color color = {1.0f, 1.0f, 1.0f, 1.0f};

    // intensity：环境光亮度标量（乘到 color 上，再乘 AO）。
    //   - intensity=1.0 ≈ 正午晴空亭子阴影的环境光 ≈ 20,000 lux。
    //   - 0 = 无环境光（物体背阳面纯黑，只剩直射光/emissive）；
    //   - 夜晚应远小于日间（如 ~0.01），否则“夜里物体太亮”；
    //   - 可 >1 做整体提亮；
    //   - 负值在 shader 里 clamp 到 0。
    float intensity = 1.0f;
};

// 色彩分级（ASC-CDL 风格，per-channel）。作用于 tone map **之前**的 HDR 值。
// 这是“光影塑造”环节（scene-referred）：
//   out = pow(max(v * slope + offset, 0), power)
//   slope（=Gain，亮部斜率）、offset（=Lift，暗部偏移）、power（=Gamma，幂指数）
// 三个分量分别作用于 R/G/B，实现分通道调色（split-tone：暗部偏青/亮部偏橙）。
// 默认全 1/0/1 + enabled=false：恒等，零回归。
struct ColorGrade {
    // 亮部斜率（Gain）。>1 提亮亮部，<1 压暗亮部。默认 1（无增益）。
    Vec3f slope  = {1.0f, 1.0f, 1.0f};

    // 暗部偏移（Lift）。>0 抬升暗部（褪色感），<0 压死暗部。默认 0。
    Vec3f offset = {0.0f, 0.0f, 0.0f};

    // 中间调幂指数（Gamma）。
    //   power=1：恒等；>1 压暗中间调（提对比）；<1 提亮中间调（降对比）。
    // 默认 1（无指数调整）。
    Vec3f power = {1.0f, 1.0f, 1.0f};

    // 是否启用分级。false（默认）时整体恒等，零回归；true 时应用以上参数。
    bool enabled = false;
};

// 辉光（Bloom）后处理参数——HDR 高亮部分提取→模糊→加回原图的机制。
//
// 背景与插入链路（见 src/renderer.cc Render()）：
//   3D 内容先渲进 HDR FBO（RGBA16F，可存 >1.0 亮度）→ resolve 到单采样
//   浮点纹理（resolve_tex_hdr_）→ [本 pass：bloom] → 统一 tone map pass。
//   bloom 在 tone map **之前**工作：从 HDR 里把超过 threshold 的高亮区域
//   抠出来，用多级半分辨率降采样 + 上采样叠加成一张"辉光图"，再按
//   intensity 加回 HDR 原图，之后才进 ACES tone map。这样高亮的"光晕"
//   才不会被 tone map 错误压缩丢失。
//
// 算法：仿 UE/Karis 标准流程——
//   1) 阈值提取（prefilter）：bright = max(hdr - threshold, 0)，只保留超阈部分。
//   2) 多级降采样（downsample）：每级分辨率减半（1/2, 1/4, 1/8, ...），
//      相邻级间既是缩小又自带一次模糊；从 level=1 取到 levels 级。
//   3) 上采样叠加（upsample accumulate）：从最小一级逐级放大回全分辨率，
//      每级和下一大的那一级做加法累加，得到多尺度辉光图。
//   4) 加回：hdr += bloom * intensity，再进 tone map。
//
// 这是一个确定性后处理（同输入必同输出），不像 PBR 光照那样有 llvmpipe
// 三稳态漂移，因此可以做逐像素 gold 验证。
struct BloomConfig {
    // 是否启用辉光。false（默认）时整个 pass 跳过，零开销、零回归。
    bool enabled = false;

    // 整体强度（线性缩放辉光颜色）。0 = 无辉光（等同关闭）；
    // 0.5~1.0 常见；>1 过曝。
    float intensity = 0.6f;

    // 亮度门槛（作用于 tone map 前的 HDR 线性值）。
    //   hdr 的亮度 luminance <= threshold 的区域不参与辉光；
    //   只有亮度 > threshold 的高亮部分被提取、模糊、加回。
    //   默认 1.0（HDR 线性亮度 >1 即视为高亮，晴天基准：太阳盘 1e3、
    //   直射光 3.0 都远超此值，普通漫反射面 <1 不辉）。
    float threshold = 1.0f;

    // 降采样级数（从 1/2 开始连续减半：level1=1/2, level2=1/4, ...）。
    // 每多一级，辉光的"光晕半径"约翻倍、效果更柔和。
    //   2~3 级：紧凑小幅辉光（适合黄金测试快速验证）。
    //   5~6 级：UE 风格的很宽光晕（性能更贵）。
    // 默认 3：给 sun_path 让太阳盘周围有一圈看得清的柔和光晕。
    int levels = 3;
};

// 全局阴影配置（级联阴影贴图 CSM）——"太阳怎么投影子"的工程参数。
//
// 与 DirectionalLight（光学参数：方向/颜色/强度，每帧在
// RenderCommandList::sun 里设）分开：阴影的切分/分辨率/淡出是场景级调参，
// 生命周期内不变，挂在 JPOV::Config::shadow 上，避免每帧重建 FBO。
//
// 段切分：
//   共 cascade_count 段。cascade_ranges[k] 是第 k 段的 far 距离，
//   第 0 段覆盖 [near(=相机near), ranges[0]]，第 k 段覆盖 [ranges[k-1], ranges[k]]。
//   cascade_ranges 必须严格递增，最后一段的 far 即总阴影距离。
//   每段有独立的 shadow map 分辨率 cascade_sizes[k]（可不同：近段大、远段小）。
//
// 淡出：
//   fade_start/fade_end 定义距相机距离区间，在此区间内影子强度线性衰减到 0
//   （fade_end 之后无影子）。淡出的是"影子强度"，独立于太阳光 intensity。
//
// ShadowConfig::Default() 提供一份面向开放世界场景的通用配置（3 段）。
// 小场景用默认值即可：近段覆盖全场景，效果等价单张 shadow map。
//
// Pre-condition: 由 JPOV::Init() → Renderer::Init 时校验（ValidateShadowConfig），
// 非法参数 LOG(FATAL) crash。
struct ShadowConfig {
    static constexpr int kMaxCascades = 5;   // shader uniform 数组长度上限

    // 默认成员值 = 开放世界通用配置（等同 Default()）。因此
    // `ShadowConfig shadow;` / `ShadowConfig{}` 即为合法可用状态，
    // 无需用户显式调 Default()。未用到的数组位（i >= cascade_count）填 0。
    int   cascade_count = 5;                            // 级联段数 [1, kMaxCascades]
    float cascade_ranges[kMaxCascades] = {7.2f, 28.8f, 64.8f, 115.2f, 180.0f};
    int   cascade_sizes[kMaxCascades]  = {2048, 1024, 1024, 512, 512};
    float fade_start = 120.0f;                // 阴影淡出起点（距相机）
    float fade_end   = 180.0f;                // 阴影淡出终点（此距离后无阴影）

    // 每级联 1 个 depth-bias 的 bias_base，**单位为米（世界单位）**，用于 slope 项：
    //   depthBias = max(0.01, cascade_bias[c] * (1-NdotL))
    //   （minBias=0.01 全局兜底垂直光 NdotL→1 使 slope 归零；slope 项管中等倾角。）
    // 与级联一一对应：cascade_bias[c] 只作用于第 c 段。外部可逐级联覆盖。
    // 默认值按“≥ 该级联单 texel 世界覆盖大小”原则设定（2026-08-30）：
    //   texel_world ≈ 级联覆盖跨度 / 分辨率；以 fov=60° 估 覆盖跨度≈1.155×far。
    //   近级联小、远级联大（因远级联分辨率低、far 大），故 bias_base 随级联递增。
    //   ⚠️ 注意：这是米单位（因 depth 已是米），远非旧版 NDC 的 0.002~0.008。
    float cascade_bias[kMaxCascades] = {0.005f, 0.033f, 0.073f, 0.260f, 0.406f};

    // 默认配置：5 级联、近处高分辨率远处低分辨率、指数分布（近密远疏）、自然淡出。
    // 级联边界用指数公式 边界(i) = 总距离×(i/N)²（UE 常用）：
    //   7.2 / 28.8 / 64.8 / 115.2 / 180.0 米。第一级较细（0~7.2m），
    // 覆盖玩家近处的精细阴影；总阴影距离 180m，超出后淡出。
    static ShadowConfig Default() { return ShadowConfig{}; }
};

// 单个级联的 shadow FBO + 深度纹理（Renderer 内部持有，UploadSunData 读取 .tex）。
// shadow 深度实为 RGBA32F 颜色纹理（存光空间 ndc.z），rb 为配套 depth renderbuffer
//（仅遮挡测试用）。纹理单元从 TEXTURE7 起绑定到 PBR shader 的 uShadowMap[i]。
struct CascadeFBO {
    unsigned int fbo = 0, tex = 0, rb = 0;
    int size = 0;
};

// 天光（Sky）指令 —— 程序化 Preetham 白天天空（DaySkyCommand）
//
// 用解析式大气模型（Preetham-Shirley-Smits 1999，见 box3d preetham.glsl）
// 计算方向非对称的**白天**天空色：太阳位置(时间) + turbidity(天气) + season(季节)
// 得到真实蓝天/晚霞的方向分布（太阳方向红橙、对面蓝黑）。非 HDRI——纯程序化、
// 参数少、连续可动画。
//
// 一帧一次，独立 sky shader program，先画（垫 3D FBO 背景）再画 3D 物体。
// 地平线以下（pitch<0 的下半球）画纯色 ground_color。
//
// ⚠️ HDR 建模：本 shader **输出 HDR 原始亮度（可 >1.0），不做任何 tone mapping**。
//    最终画面需由统一的**后处理 tone map pass**压缩到 [0,1]（见 renderer 的后处理
//    规划）。任何天空颜色/亮度都应按 HDR 量级给定，不要预选 clamp 到 [0,1]。
//
// ⚠️ 本版本为**最小可行·仅白天**：只定义“白天天空”这一天色，
//   - 不画太阳/月亮盘（天体，后续独立实现）；
//   - 不推导方向光/环境光（derive 方法已移除，2026-08-19 决定）；
//   - 不提供夜空色（太阳落山后 sky 向 (0,0,0) 逼近，夜空留待独立 layer blend）。
//   - 夜空/月光散射/辉光不是本模型职责（Preetham 在太阳低于地平线时会发散，
//     业界不扩展它建模夜晚；夜晚由独立 sky layer + 后处理补）。
struct DaySkyCommand {
    // ── 太阳位置（时间） ──
    //
    // sun_dir：太阳在天球上的位置，世界空间单位向量（y-up）。
    //   - y > 0：太阳在地平线上（白天），昼夜过渡由 y 连续控制。
    //   - y = 0：太阳恰在地平线（日出/日落）。
    //   - y < 0：太阳在地平线下（夜晚/暮光），天空只剩暮色或夜空。
    //   - 长度不必严格为 1（内部 normalize），但不要给零向量（方向未定义）。
    //   - 典型方向：正午 ≈ (0, 1, 0)；日出/日落 ≈ (≈0, 0, 任意水平方向)。
    // 决定性参数：它同时驱动 Preetham 天空的方向非对称 + 昼夜过渡(daylight)。
    jpov::Vec3f sun_dir = jpov::Vec3f(0.0f, 1.0f, 0.0f);

    // ── 天气 ──
    //
    // turbidity（大气浊度）：控制空气浑浊度，影响天空白晕/霾感。
    //   - 值越大空气越浑浊：天空从湛蓝 → 发白 → 灰霾。
    //   - 取值范围 >= 1.0（内部 clamp 到 >=1），典型晴天 2~6，重霾可到 8~12。
    //   - 建议：2.0 极清澈（高原/晴空）、4.0 正常晴天、6.0 阴霾、>8 重霾。
    //   - 独立于亮度(intensity)与昼夜(sun_dir.y)，只影响天空颜色分布。
    float turbidity = 2.0f;

    // ── 季节（色温倾向） ──
    //
    // season：作用在整个天光上的色温乘子（RGB 分量相乘）。
    //   - 只调“色调”，不调“亮度”（亮度由 intensity 单独控制）。
    //   - 各分量范围 [0, +∞)，1.0 = 中性；<1 压暗该通道，>1 增亮。
    //   - 建议：冬冷（偏青白）≈ (0.75, 0.85, 1.0)；夏暖（金黄）≈ (1.0, 0.97, 0.90)。
    Color season = {1.0f, 1.0f, 1.0f, 1.0f};

    // ── 亮度基准 ──
    //
    // intensity：天光整体亮度的统一基准标量（整体缩放天空背景亮度）。
    //   - 调小 = 画面一起变暗；调大 = 一起变亮（曝光补偿）。
    //   - 建议：1.0 = 物理基准；阴天/日食整体压暗时 <1；曝光补偿 >1。
    float intensity = 1.0f;

    // ── 地平线以下地色 ──
    //
    // ground_color：地平线以下（pitch<0 的下半球）填充的纯色。
    //   - 作用：避免地面以下透出“天空倒影”的违和感，纯视觉底色，不参与物体光照。
    //   - 建议：深灰蓝/深棕（接近地面），如 (0.05, 0.06, 0.08, 1.0)。
    Color ground_color = {0.05f, 0.06f, 0.08f, 1.0f};

    // ── 太阳盘（自发光天体，需单独给的两个参数） ──
    //
    // 太阳盘是自发光天体（5778K 黑体辐射），散射模型（Preetham）不含它，
    // 故“盘相对天空的亮度倍数”和“盘的大小”这两个量必须用户单独给，
    // 其余（颜色色温 / 随仰角衰减 / 光晕宽窄）由 sun_dir + turbidity 自动推导。
    //
    // sun_radius：太阳盘**角半径**（弧度）。物理真实 ~0.27°≈0.0047 rad，
    //   与仰角/天气无关，是显示选择。默认取物理 2 倍（~0.54°≈0.0094 rad）
    //   使盘在低分辨率下更醒目。0 或负 = 不画太阳盘。
    float sun_radius = 0.0094f;

    // sun_brightness：太阳盘**自发光亮度基数**（HDR 值）。
    //   物理依据（RP Photonics / thecalcu，2026-08-19 查证）：
    //     太阳盘亮度 ≈ 1.6×10⁹ cd/m²，晴天天空亮度 ≈ 8000 cd/m²，
    //     亮度比 ≈ 2×10⁵（两者同为 radiance/luminance 单位 cd/m²，可直接比）。
    //   故 sun_brightness 默认 = 2.0e5，表示“太阳盘比天空背景亮 ~2×10⁵ 倍”。
    //   实际盘亮度 = sun_brightness × exp(−τ·AM(仰角,浊度))（Beer-Lambert，
    //   在 shader 内推导）；颜色随仰角从 2000K(日出) 到 5600K(正午)推导。
    //   0 或负 = 不画太阳盘。
    float sun_brightness = 2.0e5f;

    // sun_glow：太阳盘**光晕强度**（艺术参数，独立于盘，用于盖锯齿 + 大气辉光感）。
    //   光晕是大气散射，物理强度远低于盘（~盘的 1e-4~1e-5），故这是**独立的绝对
    //   HDR 强度**（量级 ~几），非与盘亮度同源。光晕用标准高斯 glow ∝ exp(−d²/2σ²)，
    //   σ≈2×sun_radius（盘外 ~2~3 倍区域）。0 = 无光晕；默认 ~1.0 经验值。
    float sun_glow = 1.0f;

    // sun_set_start_angle：日盘俯仰角重映射的起始阈值（**度**，建议 [0,60]）。
    //   只影响**日盘的位置**（盘中心俯仰角），不影响天空散射/昼夜过渡/色温/衰减
    //   （那些仍由真实 sun_dir 驱动）。当太阳真仰角低于此值时，盘中心俯仰角按
    //   sun_set_angle_ratio 更快地压向/压过地平线，使盘在低仰角提前“没入地平线”，
    //   避免高仰角（尤其 90° 正午正上方）被全局偏置拉偏。≥ 此值时盘俯仰 = 真实仰角。
    //   默认 10°：太阳真仰角 < 10° 才开始压盘；≥ 10° 盘就在真实位置。
    float sun_set_start_angle = 10.0f;

    // sun_set_angle_ratio：太阳真仰角 < sun_set_start_angle 时，盘俯仰角的压速比。
    //   盘俯仰角(°) = sun_set_start_angle
    //                + sun_set_angle_ratio × (真实仰角° − sun_set_start_angle)
    //   即真实仰角每降 1°，盘俯仰降 sun_set_angle_ratio 度（>1 = 盘降得更快）。
    //   联合默认（start=10, ratio=1.4）：
    //     真实 10°  → 盘 10°（边界连续，无突变）
    //     真实  3°  → 盘 10 + 1.4×(3−10) = 0.2° ≈ 盘中心落在地平线（肉眼标定：3° 处
    //                 日盘应正好位于中心 0° 位置，被地平线淹没）
    //     真实  0°  → 盘 10 + 1.4×(0−10) = −4°（盘中心已沉到地平线下，半拉盘不再露出）
    //   默认 1.4。
    float sun_set_angle_ratio = 1.4f;

    // 色温（开尔文）→ 线性 sRGB。黑体辐射到 sRGB 的近似（Tanner Helland 拟合 +
    // 白平衡到 ~5600K 中性，再归一化）。与 sky_renderer.h 里 shader 的
    // colorTempToLinear() 逐字一致，保证 C++ 侧推导的直射光颜色与 shader 侧
    // 太阳盘颜色出自同一色温曲线。
    static Color ColorTempToLinear(float kelvin) {
        const float t = std::clamp(kelvin, 1000.0f, 40000.0f) / 100.0f;
        Color c;
        // 红通道
        if (t <= 66.0f) c.r = 1.0f;
        else c.r = std::clamp(1.29293618606f * std::pow(t - 60.0f, -0.1332047592f), 0.0f, 1.0f);
        // 绿通道
        if (t <= 66.0f)
            c.g = std::clamp(0.3900815787697696f * std::log(t) - 0.6318414437886277f, 0.0f, 1.0f);
        else
            c.g = std::clamp(1.1298908608951798f * std::pow(t - 60.0f, -0.0755148492f), 0.0f, 1.0f);
        // 蓝通道
        if (t >= 66.0f) c.b = 1.0f;
        else if (t <= 19.0f) c.b = 0.0f;
        else c.b = std::clamp(0.543206789110196f * std::log(t - 10.0f) - 1.19625408914f, 0.0f, 1.0f);
        c.a = 1.0f;
        return c;
    }

    // 太阳直射光颜色（DirectionalLight::color）。由 sun_dir 的太阳仰角推导色温
    //（正午 ~5600K 中性白 → 日出日落 ~2000K 橙红），与 sky shader 里太阳盘颜色
    // 用同一套色温曲线，保证天色与 DirectionalLight 颜色协调一致。
    //
    // 用法：让 sun（DirectionalLight）的颜色跟随 sun_dir 自动变化，而不是手配：
    //   DirectionalLight light;
    //   light.direction = <光传播方向（= -sky.sun_dir）>;
    //   light.color = sky.DirectionalColor();
    //   light.intensity = ...（强度仍独立配，本方法只管色调）
    Color DirectionalColor() const {
        // 太阳在天球上的仰角（弧度）。sun_dir 长度不必归一，取 y 前先 normalize。
        const Vec3f d = sun_dir.Unit();
        const float elev = std::asin(std::clamp(d.y(), -1.0f, 1.0f));
        // 色温（开尔文）：仰角越高色温越高。与 shader 同阈值（0.3 rad ≈ 17°）。
        const float kelvin = 2000.0f + (5600.0f - 2000.0f) *
            std::clamp(elev / 0.3f, 0.0f, 1.0f);
        const Color base = ColorTempToLinear(kelvin);
        // 季节色温偏置：乘归一化后的纯色调 season（只偏色、不改亮度，见 SeasonTint()）。
        return Color{base.r * season.r * SeasonTintScale(),
                     base.g * season.g * SeasonTintScale(),
                     base.b * season.b * SeasonTintScale(),
                     1.0f};
    }

    // 归一化 season 为“纯色温偏置”的缩放因数：把三通道同乘 1/avg(season)，
    // 使乘积不变总亮度（avg(rgb)=1），只改变 RGB 的**相对**比例 → 只偏色温、不增减光强。
    // 用法：color × season × SeasonTintScale()。season=(1,1,1) 中性 → scale=1 → 零改变。
    // assert 防 season 全零 / 负值导致 scale 非正。
    float SeasonTintScale() const {
        const float avg = (season.r + season.g + season.b) / 3.0f;
        return (avg > 0.0f) ? (1.0f / avg) : 1.0f;
    }

    // 太阳直射光强度（DirectionalLight::intensity）。由 sun_dir 仰角查注向直射
    // 辐照度（DNI）衰减表，用 PiecewiseLinearFunction 做分段线性插值。
    //
    // 物理：平行光强度代表太阳盘辉度/法向直射辐照度（DNI），非地面照度。
    // 地面照度随仰角下降已由 shader 的 N·L=cos(仰角) 自动完成，方向光本身
    // 只额外衰减“穿透大气的辐射损失”——即 DNI 的大气透过率。
    // 太阳光辉度在 20°~90° 很平缓，仅 <10° 贴地时快速趋零，
    // 这正是“平行光黄昏基本不衰减、只有贴地才消失”的原因。
    //
    // DNI 锚点（太阳仰角° → 相对正午系数），Bouguer-Lambert-Beer 光学：
    //   exp(−tau·AM)，tau≈0.39 晴空，AM 用 Kasten-Young 空气质量：
    //   0°→0  3°→0.004  5°→0.027  7°→0.073  10°→0.167  12°→0.235
    //   15°→0.334  20°→0.476  30°→0.679  45°→0.851  60°→0.942  90°→1.00
    // 正午（90°）系数=1.0，故 intensity = midday_intensity × 系数。
    // turbidity 暂时忽略（不影响方向光强度，只影响天色/太阳盘）。
    // 仰角低于 0° 时 PWL 夹断到 0（贴地趋零）；高于 90° 夹断到 1.0。
    //
    // 用法：让 sun（DirectionalLight）的强度跟随 sun_dir 自动变化，而不是固定 3.0：
    //   DirectionalLight light;
    //   light.direction = <光传播方向（= -sky.sun_dir）>;
    //   light.color = sky.DirectionalColor();
    //   light.intensity = sky.DirectionalIntensity();   // 正午=3.0，低仰角自动变暗
    float DirectionalIntensity(float midday_intensity = 2.2f) const {
        static const geom::math::PiecewiseLinearFunction<double> kSunIntensityCurve(
            // 太阳仰角(°) → 相对正午系数。基于法向直射辐照度 DNI 的
            // 大气透过率（Bouguer-Lambert-Beer：exp(−tau·AM)，tau≈0.39 晴空，
            // Kasten-Young 空气质量）。反映“太阳光辉度穿大气”的损失：
            // 20°~90° 相当平缓，只在 <10° 贴地时快速趋零。单位无需 0 点即可。
            std::vector<double>{1,  2,  5,  10,  30, 45,  90},
            std::vector<double>{0,  0.23,0.77,0.77, 0.91,  1.0, 1.0});
        const Vec3f d = sun_dir.Unit();
        const float elev_deg = std::asin(std::clamp(d.y(), -1.0f, 1.0f)) *
                               (180.0f / static_cast<float>(M_PI));
        // 基准晴空（turb=2）曲线 × 浊度衰减（turb=2 时 Loss=1.0，不改变晴空锚点）——
        // 高浊度（阴/霾）时太阳直射 DNI 大幅衰减，尤其低仰角。
        return midday_intensity * static_cast<float>(kSunIntensityCurve(elev_deg)) *
               TurbSunLoss(turbidity);
    }

    // 环境光强度（AmbientLight::intensity）。由 sun_dir 仰角查天光衰减表，
    // 用 PiecewiseLinearFunction 分段线性插值。
    //
    // 物理：ambient 是天光散射（大气把太阳光散射到整个天空）的环境补光，
    // 物理来源与方向光（DNI 直射）不同——散射光随太阳降低而变弱，但比直射
    // 衰减**缓和得多**，且在太阳落山（仰角<0）后仍有余晖（民用暮光），
    // 直到 -18°（天文暮光起）才接近全黑。故本表**不归零到 0°**，而是延伸到
    // 负仰角。
    //
    // 锚点（太阳仰角° → 相对正午系数）：
    //   90→1.00  60→0.92  45→0.84  30→0.70  20→0.55  15→0.44  12→0.38
    //   10→0.32  7→0.24  5→0.18  3→0.12  0→0.06  -3→0.03  -6→0.012
    //   -12→0.004  -18→0.001
    // 正午（90°）系数=1.0，故 intensity = noon_intensity × 系数。
    // 仰角 >90° 夹断到 1.0；< -18° 夹断到 ~0.001（夜天空底色，不归纯黑）。
    //
    // 用法：ambient 的亮度跟随天光自动变化：
    //   AmbientLight light;
    //   light.color = sky.AmbientColor();
    //   light.intensity = sky.AmbientIntensity();   // 正午=1.0，黄昏/夜晚自动变暗
    float AmbientIntensity(float noon_intensity = 1.0f) const {
        static const geom::math::PiecewiseLinearFunction<double> kSkyIntensityCurve(
            std::vector<double>{1,  2,   5,   10,  30, 45,  90},
            std::vector<double>{0.1,0.15,0.25,0.25,0.3,0.38,0.4});
        const Vec3f d = sun_dir.Unit();
        const float elev_deg = std::asin(std::clamp(d.y(), -1.0f, 1.0f)) *
                               (180.0f / static_cast<float>(M_PI));
        // 基准晴空（turb=2）曲线 × 浊度衰减（turb=2 时 Loss=1.0）——环境光是散射光
        // （DHI），随浊度衰减**远比直射光缓和**：薄云时漫射甚至略升（更多直射被
        // 散射成漫射），仅重霾才明显下降。故用独立的平缓曲线，不与 TurbSunLoss 同用。
        return noon_intensity * static_cast<float>(kSkyIntensityCurve(elev_deg)) *
               TurbAmbLoss(turbidity);
    }

    // 太阳直射光随浊度（turbidity）的整体衰减系数，以 turb=2 大晴天为基准 = 1.0。
    // 物理：太阳直射（DNI）走单一路径穿大气，衰减近似指数吸收 exp(−τ·AM)，
    // 随浊度增大强衰减（尤其低仰角）。但 JPOV 走 ACES tone map，基准落在 ACES
    // 线性区（surface radiance≈0.5），若衰减到 0.02 会进 ACES 被非线性压成死黑
    // （aces(3.0×0.02)≈贴地）。故这里用“物理趋势 + ACES 观感下限”的折中：阴天
    // 直射给到 aces 后仍能辨物的下限（turb=8 → 0.06，aces(3.0×0.06)≈0.25）。
    // turb<2 夹断到 1.0（更通透的天不增亮）；turb>8 夹断到 0.06。
    //
    // 锚点（turb → 相对 turb=2 大晴的直射衰减，ACES 观感折中）：
    //   2→1.00(基准)  3→0.80  4→0.60(薄云)  5→0.40  6→0.22  8→0.06(阴天)
    float TurbSunLoss(float turb) const {
        static const geom::math::PiecewiseLinearFunction<double> kTurbSun(
            std::vector<double>{2.0, 3.0, 4.0, 5.0, 6.0, 8.0},
            std::vector<double>{1.00, 0.80, 0.60, 0.40, 0.3, 0.2});
        return static_cast<float>(kTurbSun(turb));
    }

    // 环境光随浊度（turbidity）的整体衰减系数，以 turb=2 大晴天为基准 = 1.0。
    // 物理：环境光是天光漫射（DHI），非单一路径，故随浊度衰减远比直射缓和，
    // 且趋势不同——薄到中云时更多直射被散射成漫射，DHI 不降反微升（>1.0）；
    // 到重霾/厚云时总量才明显下降。故这条曲线平缓，不与 TurbSunLoss 同用。
    // turb<2 夹断到 1.0；turb>8 夹断到 0.5。
    //
    // 锚点（turb → 相对 turb=2 大晴的环境光衰减）：
    //   2→1.00(基准)  3→1.08  4→1.05  5→0.90  6→0.70  8→0.50(阴天)
    float TurbAmbLoss(float turb) const {
        static const geom::math::PiecewiseLinearFunction<double> kTurbAmb(
            std::vector<double>{2.0, 3.0, 4.0, 5.0, 6.0, 8.0},
            std::vector<double>{1.00, 1.08, 1.05, 0.90, 0.70, 0.50});
        return static_cast<float>(kTurbAmb(turb));
    }

    // 环境光颜色（AmbientLight::color）——天光平均色。
    // 正午晴天天空偏蓝（高色温 ~15000K）；黄昏太阳低，散射光转橙（低色温）；
    // 太阳落山后继续转深橙（暮光）。与 DirectionalColor 用同一套色温曲线
    // （ColorTempToLinear），但色温映射区间不同（天光比太阳盘冷/蓝）。
    //
    // 色温锚点（太阳仰角° → 天光色温 K）：
    //   90→15000  60→11167  45→9250  30→7333  20→6056  12→5033
    //   5→4140  0→3500  -3→3275  -6→3050  -12→2600  -18→2150
    // 仰角 >90° 夹断到 15000K；< -18° 夹断到 2150K。
    //
    // turbidity 影响：浊度大（霾/烟雾）→ 大气散射更强 → 蓝天发白、饱和度下降，
    // 故把天光色往中性灰白插值。雾度系数 clamp((turb-2)/6)∈[0,1]（turb=2 晴→0，
    // turb=8 重霾→1），插值限幅 0.6（不完全变白，保留底色）。
    Color AmbientColor() const {
        static const geom::math::PiecewiseLinearFunction<double> kSkyTempCurve(
            std::vector<double>{-18.0, -12.0, -6.0, -3.0, 0.0, 5.0, 12.0, 20.0, 30.0, 45.0, 60.0, 90.0},
            std::vector<double>{2150.0, 2600.0, 3050.0, 3275.0, 3500.0, 4139.0, 5033.0, 6056.0, 7333.0, 9250.0, 11167.0, 15000.0});
        const Vec3f d = sun_dir.Unit();
        const float elev_deg = std::asin(std::clamp(d.y(), -1.0f, 1.0f)) *
                               (180.0f / static_cast<float>(M_PI));
        const float temp = static_cast<float>(kSkyTempCurve(elev_deg));
        const Color tint = ColorTempToLinear(temp);
        // 浊度 → 雾度系数：turb=2 晴无雾，turb=8 重霾趋向发白。
        const float haze = std::clamp((turbidity - 2.0f) / 6.0f, 0.0f, 1.0f) * 0.6f;
        const Color white = {0.9f, 0.9f, 0.9f, 1.0f};
        const Color c{
            tint.r * (1.0f - haze) + white.r * haze,
            tint.g * (1.0f - haze) + white.g * haze,
            tint.b * (1.0f - haze) + white.b * haze,
            1.0f,
        };
        // 季节色温偏置：乘归一化后的纯色调 season（只偏色、不改亮度），
        // 与 DirectionalColor 用同一 SeasonTintScale()，保证天空与物体受光色调一致。
        return Color{c.r * season.r * SeasonTintScale(),
                     c.g * season.g * SeasonTintScale(),
                     c.b * season.b * SeasonTintScale(),
                     1.0f};
    }
};

// 3D 静态模型（世界空间，参与深度测试）
//
// 渲染一个已注册的 GPU mesh（见 gpumesh.h / RegisterMesh），
// 用 PBRMaterial 定义材质。base_color_tex != 0 时 baseColor 走逐像素纹理采样
//（mesh 需含 kUV + kNormal 属性，采样结果作为 GGX BRDF 的 base 色），
// 否则用 base_color 常值 fallback。
// mesh_id 为运行期由 RegisterMesh 分配的句柄。
//
// 变换约定：模型在局部空间定义，通过 center（平移）+ up/front（旋转）放置。
//   - 局部 +Y → 世界 up；局部 +Z → 世界 front；局部 +X = normalize(cross(up, front))
//   - 无缩放、不含逐物体透视；MVP = Proj * View * Model
//
// 光照：使用 GGX PBR 着色（需 mesh 含 kNormal 属性）。材质来自 material：
// baseColor 支持纹理（base_color_tex != 0，需 kUV）或常值；
// metallic/roughness/emissive/AO 各通道支持常值或纹理（见对应 *_tex 标志）。
// object_use_default_color == true 时跳过点光源（ambient-only，等价原纯色行为）。
//
// Pre-condition: mesh_id 已注册且未释放
// Pre-condition: base_color_tex == 0，或已注册且 mesh 含 kUV 属性
// Pre-condition: up、front 均非零且不平行
struct Object3DCommand {
    uint32_t mesh_id;      // 已注册的 GPU mesh 句柄
    PBRMaterial material;  // PBR 材质（常值 or 纹理通道）
    Vec3f center;          // 模型中心世界坐标（平移）
    Vec3f up;              // 局部 +Y 指向的世界方向（归一化处理）
    Vec3f front;           // 局部 +Z 指向的世界方向（归一化处理）
    float  scale = 1.0f;   // 整体缩放（先缩放顶点，再 up/front 旋转 + center 平移）

    // picking_id：该物体在 GPU color-ID 拾取中对外暴露的句柄。
    //   = 0    ：本物体**不可拾取**（不参与 picking pass，也不会被命中）。
    //   > 0    ：本物体可被拾取；拾取命中该物体时，JPOV::last_pick().picking_id
    //            回传此值。用户自行保证其唯一性（同一帧推两个相同 id 的物体，
    //            命中无法区分）。
    // 仅当 RenderCommandList::PickQuery::enabled 时才跑 picking pass；
    // 未发起查询时 picking_id 完全不产生渲染开销。
    uint32_t picking_id = 0;

    // highlight：是否为本物体绘制高亮纯色边框（方法 B：stencil + 顶点外扩）。
    //   边框颜色/线宽取全局 RenderCommandList::highlight_style（无该值时忽略）。
    //   仅当 highlight_style 有值且本物体 highlight==true 时才多画一个描边 pass；
    //   未开启高亮时零额外开销。
    bool highlight = false;
};

// 3D 骨架蒙皮模型（世界空间，instancing 批）
//
// 一批「同 rest mesh + 同骨架模板」的实例，共用一份蒙皮几何 —— 对应架构文档
// docs/jpov_crowd_instancing_arch.md §6.2-B 骨骼动画纹理：把每根骨头 JointMatrix 按
// clip→帧烘焙进一张 RGBA 纹理，instance 只送帧号/相位，蒙皮 VS 查表 + 保留 4-bone
// （顶点 4-bone 权重来自 VBO loc3/4，见 gpumesh.h）。一命令 = 一次 instanced draw
// （千人压 draw-call，是本子系统的核心诉求）。
//
// 资源边界：CPU 侧描述在 interface/skeleton_types.h（SkeletonTemplate / SkeletonClip /
// SkinnedInstanceState）；把片断注册 GPU + 烘焙动画纹理由 src/skeleton/skeleton_manager.h
// 的 SkeletonManager 持有。渲染时 shadow/picking/highlight 对该批用与主 pass 同一套蒙皮
// 查表，保证“身体动、影/拾取对得上”（避免幽灵错位）。
//
// ⚠️ 同批需共享同一 mesh_id：OpenGL 顶点几何绑定在 VAO 全体共享，不能在一个 instanced
// draw 里 per-instance 换几何。换“部位网格 / 装备”= 另发一个 SkinnedMeshCommand（同
// skeleton，别的 mesh_id）；肉体高/低模 = 各自一次 SkinnedMeshCommand（仍都是 batch）。
//
// S0：蒙皮到骨架 rest / 静态姿态（每实例 clip 用 0）；动画 clip→帧骨骼动画纹理的查表与
// instanced divisor 上传是 S0 之后的一个里程碑（见 skeleton_types.h / SkeletonManager 的
// TODO）。
// Pre-condition: mesh_id / skeleton_id 均已注册且未释放；instances 大小 >= 1（空＝不画）。
struct SkinnedMeshCommand {
    uint32_t mesh_id;        // rest mesh（reuse GPUMesh：loc0=pos, loc3=joints, loc4=weights）
    uint32_t skeleton_id;    // 登记过的骨架模板（含逆绑定 constant），由 SkeletonManager 分配；
                             // 0 = 未登记（实现应 LOG(FATAL)/忽略）。
    std::vector<SkinnedInstanceState> instances;  // 这批实例。每实例自己的动画(clip_id/相位)在
                             // SkinnedInstanceState(见 skeleton_types.h)，S0 全用 clip=0=rest。
};

// 高亮纯色边框的全局样式（全场景统一）。
// 当前实现（CPU 屏幕空间回填）：GPU 把被高亮物体画成不扩张的剪影（单色 mask），
// CPU 读回 mask 做 outline_px 次像素膨胀，膨胀图与原 mask 相减得到边缘环，
// 再在屏幕空间用 GL_POINTS 回填边框色。边框为**恒定像素宽**，不随物体大小/
// 远近变化。
struct HighlightStyle {
    Color color = {1.0f, 0.85f, 0.3f, 1.0f};  // 边框颜色（金黄，经统一 tone map）

    // 边框宽度：屏幕空间**像素**（恒定线宽，不随物体大小/远近变化）。
    //   实现：对物体剪影 mask 做 outline_px 次十字膨胀（上下左右±1），
    //   膨胀图与原点集相减得到边缘环，回填为边框色。
    //   例：outline_px = 2 → 边框约 2 像素宽（上下左右各伸 2 像素）。
    // 取代旧的“模型空间放大副本”方案（旧式边框为相对缩放、近大远小、
    // 大物体边框粗；且对细长/复杂网格会走形）。
    // Pre-condition: outline_px > 0
    int outline_px = 2;
};

// 拾取查询：用户在下帧声明“我想拾取渲染分辨率上的这个屏幕点”。
// 当 enabled=true 时，Renderer 在 Render() 内对**所有 picking_id>0** 的物体
// 跑一个 color-ID pass + 读回，结果写入 JPOV::last_pick()（供下帧 OneIteration 读取）。
// enabled=false（默认）时不跑任何额外 pass，零开销。
struct PickQuery {
    // 是否发起本次拾取查询。false = 不跑 picking pass（零成本）。
    bool enabled = false;

    // screen_x, screen_y：被拾取的屏幕像素坐标。
    // ⚠️ 语义为 **窗口（Window）像素坐标**，原点在窗口左上角（x→右，y→下），
    //    与 InputSnapshot::mouse_x/mouse_y 同一坐标系，便于鼠标投影直接传入。
    //    渲染分辨率与窗口尺寸不同时，由 Renderer 内部映射到 3D FBO 的像素。
    float screen_x = 0.0f;
    float screen_y = 0.0f;
};

// 一次拾取查询的结果（渲染时经 color-ID pass + glReadPixels 得到，
// 落在 JPOV::last_pick()，供用户下帧 OneIteration 读取）。
struct PickResult {
    // 是否命中任何物体（落到某个 picking_id>0 的物体表面上）。
    // false = 命中背景/空（该屏幕点没有可拾取物体）。
    bool hit = false;

    // 命中物体的 picking_id（与触发该物体的 Object3DCommand::picking_id 一致）。
    // hit==false 时此值为 0（背景）。
    uint32_t picking_id = 0;
};

// ==================== 渲染指令列表 ====================

// 帧级输出：有序的绘制指令集合
//
// 不同的命令类型分别存储在各自的 vector 中，
// order 队列声明绘制顺序（先 3D 后 2D，画家算法）。
//
// 渲染顺序：
//   1. 所有 3D 指令（按 order 顺序，由深度测试自动处理遮挡）
//   2. 所有 2D 指令（按 order 顺序，后画覆先画，无深度测试）
struct RenderCommandList {
    // 各类命令的存储池
    std::vector<Polyline2DCommand> polyline2d;
    std::vector<Rect2DCommand> rect2d;
    std::vector<Circle2DCommand> circle2d;
    std::vector<Text2DCommand> text2d;
    std::vector<Line3DCommand> line3d;
    std::vector<Triangle3DCommand> triangle3d;
    std::vector<Strip3DCommand> strip3d;
    std::vector<Text3DCommand> text3d;
    std::vector<RoundRect2DCommand> roundrect2d;
    std::vector<FillRect2DCommand> fillrect2d;
    std::vector<Strip2DCommand> strip2d;
    std::vector<Arc2DCommand> arc2d;
    std::vector<Image2DCommand> image2d;
    std::vector<Object3DCommand> object3d;
    // 3D 骨架蒙皮批量实例命令（世界空间, instancing）。存一批 per-instance，渲染时归成一次次
    // instanced draw。每命令引用的 skeleton_id 由用户经 SkeletonManager 登记（含逆绑定/动画烘焙）。
    std::vector<SkinnedMeshCommand> skinned_mesh;
    // TODO(2026-09-06): 若按 body slot 把肉体/装备拆多份 mesh → 每份一个 SkinnedMeshCommand
    //    即可（同 mesh 才能同批 instancing）；该池只在此层存“同 mesh+skeleton 批”。
    //    高/低模 LOD 各一份 mesh=各一份命令(still 一次 batch instanced draw)，归用户可见性层切。

    // 绘制顺序队列：(类型, 索引)
    // 例如 order[0] = {kPolyline2D, 0} 表示先绘制 polyline2d 中的第 0 条
    // order[1] = {kText2D, 2} 表示再绘制 text2d 中的第 2 条
    std::vector<std::pair<DrawCommandType, int>> order;

    // 点光源列表（世界空间）。
    // 每帧可设置 0~N 个点光源，渲染时按 GGX PBR 模型计算光照。
    // 空列表时无光照效果（物体呈纯黑，仅 ambient 项可见）。
    //
    // 光源数量上限：255 个（仅前 255 个生效，超出部分静默忽略，
    // 渲染器会 LOG 一条 warning）。这是 tile 纹理 uint8 编码的硬限制。
    //
    // 渲染采用 CPU 端 tile culling（16×16 像素 tile，每个 tile 最多结算
    // 16 个影响最大的光源，先到先得按本列表顺序）；每个 fragment 只遍历
    // 本 tile 命中的光源，而非全部光源。因此请按重要性（从高到低）排列
    // 本列表，靠前的光源优先被选中。
    //
    // culling 是保守近似而非精确裁剪：光源 linear_radius 覆盖的球体被
    // 投影到屏幕做 tile 标记；当光源球跨越相机或在临界位置时，会保守地
    // 让该光源覆盖更大范围（甚至全屏）以保证不漏光。因此“某 light 影响
    // 某 tile”是保守判定，可能比实际影响范围更宽。
    std::vector<PointLight> point_lights;

    // 全局平行光（太阳）。未设置时无方向光（不产生直射高光与影子）。
    // 有值时 Renderer 额外做一次正交 shadow pass，PBR shader 采样阴影贴图
    // 并对直射光施加阴影因子。
    std::optional<DirectionalLight> sun;

    // 全局环境光。未设置时使用默认值（中性灰白 × 0.4），后续可由 DaySkyCommand
    // 自动推导。有值时按用户给定的颜色/强度照亮物体背阳面（无方向、无影子）。
    std::optional<AmbientLight> ambient;

    // 天球背景。有值时在 3D 物体之前绘制 HDR 环境贴图作为背景，
    // 姿态由 sun.direction 定死（无 sun 时用 HDRI 原方向）。
    std::optional<DaySkyCommand> sky;

    // Object3D 跳过点光源（默认 false）。
    // 为 true 时跳过 tile lighting，走 ambient-only PBR 着色（等价原纯色路径）；
    // 为 false 时使用完整 GGX PBR 光照（需 mesh 含 kNormal）。
    bool object_use_default_color = false;

    // Object3D 点光源 tile culling 开关（默认 true = 开启）。
    //   true ：CPU 端构建 16×16 tile 索引纹理，片元只结算本 tile 命中的光源
    //          （高效，但 tile 边界处光源取舍不同会产生分界线伪影）。
    //   false：跳过 tile 构建，片元遍历全部点光源（经典前向光照，
    //          无分界线，适合光源少或调试）。
    bool tile_culling = true;

    // 统一后处理 tone mapping 开关（默认 true = 开启）。
    //   3D 内容（天空 + object3d + primitives3d）先渲染进 HDR FBO（RGBA16F，
    //   可存 >1.0 的 HDR 亮度）。渲染完成后由统一的全屏后处理 pass 消费：
    //   - true ：走 ACES filmic tone map，把 HDR 亮度压缩到 [0,1] 后写入 LDR。
    //            （默认，HDR 架构的正确链路，见 LIGHT_INTENSITY.md 晴天基准值）
    //   - false：不走 tone map，直接 blit 到 LDR（HDR 值被 RGBA8 clamp，
    //            仅作调试/ before-after 对比用）。
    bool tone_mapping = true;

    // 后处理输出 sRGB 编码开关（默认 true = 开启）。
    //   - true ：tone map 输出的线性 LDR 值在写屏前做 sRGB 编码
    //            （IEC 61966-2-1，分段 lin→srgb 映射），预补偿显示器的
    //            非线性伽马，让屏幕上看到的亮度和线性空间真值一致。
    //            正确 HDR→显示链路：tone map→lin→sRGB encode→屏。
    //   - false：直接写线性 LDR 值（被 RGBA8 clamp 且被屏伽马压暗，
    //            仅作调试/ before-after 对比用）。
    // 注意：此开关只影响统一 tone map pass 的输出路径；LDR 旧路径（
    // tone_mapping=false）与后叠加的 2D/UI 内容不经过此编码（UI 颜色是
    // 设计时的 sRGB 人眼值，不应被二次变换）。
    bool srgb_encode = true;

    // 最终亮度/对比度微调（作用于 tone map + sRGB 编码之后的最终 LDR 值）。
    // 这是 post-tonemap 的观感调整，输入/输出均为 [0,1] sRGB 值：
    //   adjusted = (v - 0.5) * contrast + 0.5 + brightness
    //   contrast 围绕中灰 0.5 缩放：1.0 不变，>1 增大对比，<1 减小对比
    //   brightness 整体偏移：0.0 不变，>0 提亮，<0 压暗
    // 默认 contrast=1.0 / brightness=0.0（无调整，全链路等效不加此步）。
    float final_contrast   = 1.0f;
    float final_brightness = 0.0f;

    // 曝光（photometric exposure，固定 EV）。作用于 tone map **之前**的 HDR
    // 线性值：exposed_hdr = hdr * exposure，决定“把多大范围 HDR 亮度映射到
    // tone map 的工作区间”。
    //   exposure = 2^EV；EV=0 → 1.0（无曝光，物理锚点基准）；>1 提亮，<1 压暗。
    // 这是 fixed/manual EV（非自动曝光），不破坏物理光照锚点（LIGHT_INTENSITY.md）。
    // 默认 1.0（EV=0）：零回归，等同于当前物理标定亮度。
    float exposure = 1.0f;

    // 色彩分级（ASC-CDL 风格，作用于 tone map **之前**的 HDR 值，per-channel）。
    // 这是“光影塑造”环节，在摄影域（scene-referred）调整明暗与对比：
    //   out = pow(max(v * slope + offset, 0), power)
    //   slope（=Gain，亮部斜率）、offset（=Lift，暗部偏移）、power（=Gamma，幂指数）
    // 三个分量分别作用于 R/G/B，可实现分通道调色（如 split-tone：暗部偏青/亮部偏橙）。
    // enabled=false（默认）时不做任何分级，恒等零回归。
    ColorGrade grade;

    // 辉光参数。有值且 bloom.enabled==true 时，在 tone map **之前**把 HDR
    // 高亮区域提取、模糊、加回原图（见 BloomConfig 注释）。
    // 默认无值（等同关闭），零额外开销、零回归。
    std::optional<BloomConfig> bloom;

    // 全局高亮样式。有值且某 Object3DCommand::highlight==true 时，
    // 给该物体绘制方法 B 纯色边框（stencil + 顶点外扩）。
    // 无值（默认）时不绘制任何高亮，零额外开销。
    std::optional<HighlightStyle> highlight_style;

    // 拾取查询。enabled==false（默认）时不跑 picking pass，零开销；
    // enabled==true 时对 picking_id>0 的物体跑一次 color-ID pass，
    // 结果落在 JPOV::last_pick() 供下帧读取。
    PickQuery pick;

    // 3D 透视相机
    // 每帧有且仅有一个 Camera，用户在 OneIteration 中设置此字段。
    // 框架在 Render() 时自动使用该 Camera 计算 MVP 变换。
    // 若无需 3D 渲染可保持默认值（此时 3D 绘制结果未定义）。
    Camera camera;

    // 清空本帧所有指令（框架在每帧开始时调用）
    void Clear();

    // ---- 2D 绘制辅助方法（屏幕空间，像素坐标） ----

    // 2D 折线（方角端点）
    // vertices: 折线的顶点序列
    // color: 整条线统一颜色
    // Pre-conditions:
    //   - vertices.size() >= 2
    //   - vertices.size() - 1 <= Renderer::kMaxPolylineEdges
    //   - line_width > 0
    void DrawPolyline(const std::vector<Vec2f>& vertices, const Color& color,
                      float line_width = 1.0f);

    // 2D 实心矩形（渲染分辨率空间，像素坐标）
    //
    // pos:  矩形左上角位置（像素坐标，原点在渲染分辨率左上角，x→右，y→下）
    // size: 矩形的宽度和高度（像素单位，>0）
    // color: 填充颜色（RGBA，分量范围 [0,1]）
    //
    // 坐标空间与 camera.fbo_3d_width_/height_ 一致。
    // 例：fbo_3d_width_=640, fbo_3d_height_=360 时，
    //     DrawRect({160,90},{320,180},blue) 画一个居中矩形。
    // Pre-condition: pos_x >= 0, pos_y >= 0
    // Pre-condition: size.x > 0, size.y > 0
    void DrawRect(const Vec2f& pos, const Vec2f& size, const Color& color);

    // 2D 实心圆
    // Pre-condition: radius > 0
    void DrawCircle(const Vec2f& center, float radius, const Color& color);

    // 2D 文本
    // Pre-condition: font_size > 0
    // pos: 文本位置（像素单位，含义取决于 alignment）
    // alignment: 文本对齐方式（缺省 kTopLeft，保持向后兼容）
    // font_alias: 字体别名（与 JPOV::Config::FontEntry::alias 对应），必填
    void DrawText(const std::string& text, const Vec2f& pos, float font_size,
                  const Color& color,
                  TextAlignment alignment,
                  const std::string& font_alias);

    // ---- 3D 绘制辅助方法（世界空间，右手系） ----

    // 3D 线段
    // Pre-condition: width > 0
    void DrawLine3D(const Vec3f& p1, const Vec3f& p2, const Color& color,
                    float width = 1.0f);

    // 3D 实心三角形（参与深度测试）
    void DrawTriangle3D(const Vec3f& p1, const Vec3f& p2, const Vec3f& p3,
                        const Color& color);

    // 3D 条带（VBO 加速）
    // 顶点由条带化规则生成三角形 (p0p1p2, p1p2p3, ...)
    // Pre-condition: vertices.size() >= 3，否则忽略
    void DrawStrip3D(const std::vector<Vec3f>& vertices,
                     const Color& color);

    // 3D 文本（面向摄像机标签，参与深度测试）
    // Pre-condition: font_size > 0
    // font_alias: 字体别名（与 JPOV::Config::FontEntry::alias 对应），必填
    void DrawText3D(const std::string& text, const Vec3f& pos, float font_size,
                    const Color& color,
                    const std::string& font_alias);

    // ---- 2D 条带辅助方法 ----

    // 2D 条带（屏幕空间，像素坐标，GL_TRIANGLE_STRIP）
    // 顶点由条带化规则生成三角形 (p0p1p2, p1p2p3, ...)
    // Pre-condition: vertices.size() >= 3，否则忽略
    void DrawStrip2D(const std::vector<Vec2f>& vertices,
                     const Color& color);

    // 2D 圆角矩形（渲染分辨率空间，像素坐标）
    //
    // 利用 CPU 端圆角三角化产生顶点数据后渲染。
    // 圆角半径不能超过矩形短边的一半。
    // Pre-condition: size.x > 0 && size.y > 0
    // Pre-condition: radius >= 0
    // Pre-condition: radius <= min(size.x, size.y) / 2
    void DrawRoundRect(const Vec2f& pos, const Vec2f& size,
                       float radius, const Color& color);

    // 2D 复合矩形（渲染分辨率空间，像素坐标）
    //
    // 带填充色、边框颜色/宽度、圆角半径的复合矩形。
    // 边框宽度为 0 时仅绘制填充部分。
    // 利用 RoundRect2D + Strip2D 组合实现。
    // Pre-condition: size.x > 0 && size.y > 0
    // Pre-condition: radius >= 0
    // Pre-condition: radius <= min(size.x, size.y) / 2
    // Pre-condition: border_width >= 0
    void DrawFillRect(const Vec2f& pos, const Vec2f& size,
                      const Color& fill_color,
                      const Color& border_color,
                      float border_width, float radius);

    // 2D 圆弧/扇形（渲染分辨率空间，像素坐标）
    //
    // 利用 CPU 端三角化产生三角形列表后渲染。
    // 跨度角度绝对值 >= 360 度时绘制完整圆形。
    // Pre-condition: radius > 0
    // Pre-condition: start_angle, span_angle 不受限（支持跨多圈）
    void DrawArc2D(const Vec2f& center, float radius,
                   float start_angle, float span_angle,
                   const Color& color);

    // 2D 图片（渲染分辨率空间，像素坐标）
    //
    // 将已注册的纹理绘制到指定矩形区域。
    // Pre-condition: texture_id 已注册且未释放
    // Pre-condition: size.x > 0 && size.y > 0
    void DrawImage(uint32_t texture_id, const Vec2f& pos,
                   const Vec2f& size,
                   const Color& tint = kColorWhite);

    // ---- 3D 静态模型 ----------------

    // 3D 静态模型（世界空间，参与深度测试）
    //
    // 渲染一个已注册的 GPU mesh，支持纯色或纹理着色。
    // 模型在局部空间定义（如 OBJ 坐标），通过 center（平移）与
    // up/front（旋转）放置到世界空间：
    //   - 模型的局部 +Y 轴 → 世界空间 up 方向
    //   - 模型的局部 +Z 轴 → 世界空间 front 方向
    //   - 局部 +X 轴由 up/front 叉积确定（保证右手系）
    //
    // 着色行为：DrawObject3D 统一使用 GGX PBR 着色（需 mesh 含 kNormal）。
    // object_use_default_color 仅在 Render() 中控制是否跳过点光源 tile lighting，
    // 设为 true 时 ambient-only（无直接光照，等价于原纯色路径）。
    //
    // 以 PBRMaterial 材质绘制一个 3D 静态模型（mesh 需已注册）。
    //
    // mat: PBR 材质。base_color_tex != 0 时 baseColor 走逐像素纹理采样
    //      （mesh 需含 kUV + kNormal 属性，采样结果作为 GGX BRDF 的 base 色）；
    //      否则各通道取 mat 的常值 fallback（含光照模式下的
    //      metallic / roughness / emissive 恒走常值）。
    //
    // center: 模型中心的世界坐标（平移）
    // up:     模型局部 +Y 指向的世界方向（需非零，会被归一化）
    // front:  模型局部 +Z 指向的世界方向（需非零，会被归一化）
    //
    // Pre-condition: mesh_id 已通过 RegisterMesh 注册且未释放
    // Pre-condition: base_color_tex 为 0，或已注册且 mesh 含 kUV 属性
    // Pre-condition: up 与 front 均非零向量，且不平行
    void DrawObject3D(uint32_t mesh_id, const PBRMaterial& mat,
                      const Vec3f& center,
                      const Vec3f& up, const Vec3f& front,
                      uint32_t picking_id = 0,
                      bool highlight = false,
                      float scale = 1.0f);

    // 便捷：绘制整个 glTF 对象（Renderer::LoadGltf 的产物）。
    //
    // 对 obj.primitives 中每个 primitive 展开为一个 DrawObject3DCommand，
    // 用同一个 center/up/front 放置（整个 glTF 场景作为一个整体摆放）。
    // scale：整体缩放（先缩放顶点，再 up/front 旋转 + center 平移），透传给每个 primitive。
    // 不引入新的渲染命令体 —— 内部就是多个 Object3DCommand。
    //
    // 注意: 不检查/释放 obj 的 GPU 资源；释放用 Renderer::ReleaseGltf。
    // Pre-condition: obj 由 Renderer::LoadGltf 生成，且尚未释放
    void DrawGltfObject(const GltfObject& obj,
                        const Vec3f& center,
                        const Vec3f& up, const Vec3f& front,
                        uint32_t picking_id = 0,
                        bool highlight = false,
                        float scale = 1.0f);

    // 3D 骨架蒙皮批量（instancing）—— 「这些人用这套骨架在某个动作/相位,每个人都摆在这里」。
    //
    // 语义：画“同一 rest mesh(mesh_id) 被同一个骨架模板(skeleton_id)蒙皮”的一批
    // 实例(instances)。每个实例的摆放 + 动画相位在 SkinnedInstanceState 里(见
    // skeleton_types.h)，因此 CPU/帧 每实例只传一个很薄的 per-instance state ——
    // 人群主体 S0 即靠它把上千个 instance 塞进几次 instanced draw。
    // S0 = 静态/rest 蒙皮（每实例 SkinnedInstanceState::clip_id = 0，四 pass 与主 pass 用同一套
    //   蒙皮查表保证几何一致）；动画 clip→帧骨骼动画纹理查表为 S0 之后的一个里程碑
    //   （见命令体注释 / TODO）。
    //
    // 用同一份 skeleton_id + mesh 但想体现“换部位网格”(肉体/小臂/头…)/装备 → 那是另一批
    // 各自不同 mesh_id 的 SkinnedMeshCommand；层/重要性/LOD 归用户。S0 目标“肉体高低模各
    // 一批”就是两次 DrawMeshWithSkeleton 各带不同 mesh_id。
    //
    // Pre-condition: mesh_id / skeleton_id 均已注册未释放；instances 非空。
    // TODO(2026-09-06): 首个实现建议 = 静态蒙皮(S0 零动画退化门)：clip_id=0 时不查
    //   动画纹理、用逆绑定 constant 把 rest 顶点蒙皮到骨架 rest 姿态,即现有“不带蒙皮 VS”的
    //   自然扩展(每实例只在 transform/相位上批量)。动画烘焙查表 + instanced divisor 上传
    //   随骨架动画纹理 pass 落地。
    void DrawMeshWithSkeleton(uint32_t mesh_id,
                              uint32_t skeleton_id,
                              std::vector<SkinnedInstanceState> instances);
};

}  // namespace jpov

#endif  // JPOV_RENDER_COMMAND_H_
