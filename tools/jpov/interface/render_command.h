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

#include <cstdint>
#include <string>
#include <vector>
#include <utility>

#include <glog/logging.h>
#include "geom/common/vec.h"

#include "tools/jpov/interface/camera.h"

namespace jpov {

// ==================== 类型别名 ====================

// 复用 geom 库的向量类型
// Vec2f 用于屏幕空间（像素坐标），Vec3f 用于世界空间
using Vec2f = geom::Vec2<float>;
using Vec3f = geom::Vec3<float>;

// ==================== 内置字体别名 ====================

// JPOV 内置默认字体的别名常量。
// 用户可在此查看内置字体名称，或在 DrawText 中引用。
// 如果用户通过 JPOV::Config::fonts 注册了同名 alias，初始化时会 crash。
// 注意：stb_truetype 对 CFF (PostScript outline) 格式支持不稳定，
//       仅 TrueType outline (.ttf/.ttc) 字体可用。
inline constexpr const char* kFontBuiltinCJK   = "CJK";   // NotoSansCJK-Regular.ttc (TTC index=0)
inline constexpr const char* kFontBuiltinLatin = "Latin"; // DejaVuSans.ttf

// ==================== 颜色 ====================

// RGBA 颜色，分量 [0, 1]
struct Color {
    float r, g, b, a;
};

// 常用颜色常量
extern const Color kColorRed;
extern const Color kColorGreen;
extern const Color kColorBlue;
extern const Color kColorWhite;
extern const Color kColorBlack;
extern const Color kColorTransparent;

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
    std::vector<Strip2DCommand> strip2d;

    // 绘制顺序队列：(类型, 索引)
    // 例如 order[0] = {kPolyline2D, 0} 表示先绘制 polyline2d 中的第 0 条
    // order[1] = {kText2D, 2} 表示再绘制 text2d 中的第 2 条
    std::vector<std::pair<DrawCommandType, int>> order;

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
};

}  // namespace jpov

#endif  // JPOV_RENDER_COMMAND_H_
