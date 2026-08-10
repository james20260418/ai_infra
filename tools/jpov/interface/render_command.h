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
// Pre-condition: linear_radius > 0
struct PointLight {
    Vec3f position;
    Color color;
    float linear_radius;  // 线性衰减的最大有效距离

    // 有效半径（供 culling 等使用）。
    // 当前线性衰减模式下直接返回 linear_radius；
    // 后续支持其他衰减函数时可通过此接口区分。
    float effective_range() const { return linear_radius; }
};

// PBR 材质参数（世界空间 3D 物体着色用）
//
// 描述一个 3D 物体的 PBR 材质。每个通道要么用常值（fallback），
// 要么用纹理（逐像素材质参数场，采样后进 BRDF）。
// 通道与对应是否有纹理的关系由各字段表达：
//   - base_color / emissive / ao 为颜色，分别配套 *_tex（0 = 无纹理）
//   - metallic / roughness 为标量，分别配套 has_*_tex + *_tex
//   - normal_scale 缩放法线贴图扰动强度
//
// 约定：纹理句柄 0 表示无纹理，此时使用对应的常值 fallback。
//       *_tex 非 0 时采样该纹理作为逐像素材质参数，忽略对应常值。
struct PBRMaterial {
    // baseColor: 值 或 纹理（has_*_tex 语义下用纹理）
    Color base_color;            // fallback 值
    uint32_t base_color_tex = 0; // 0 = 无纹理

    // metallic / roughness: scalar or texture
    float metallic = 0.0f;
    bool has_metallic_tex = false;
    uint32_t metallic_tex = 0;
    float roughness = 1.0f;
    bool has_roughness_tex = false;
    uint32_t roughness_tex = 0;

    // 法线贴图（扰动法线，采样的 TBN 变换）
    float normal_scale = 1.0f;
    uint32_t normal_tex = 0;     // 法线贴图

    // emissive: color or texture
    Color emissive{0.0f, 0.0f, 0.0f, 1.0f};   // 默认无自发光
    uint32_t emissive_tex = 0;

    // 烘焙 AO（先留槽）: color or texture
    // 常值取 .r 作为标量强度（灰度）；默认 1.0 = 无遮蔽，不影响环境光。
    Color ao{1.0f, 1.0f, 1.0f, 1.0f};
    uint32_t ao_tex = 0;
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
// 光照：当 RenderCommandList::object_use_default_color 为 false（默认）时，
// 模型使用 GGX PBR 光照着色（需 mesh 含 kNormal 属性）。材质来自 material：
// baseColor 支持纹理（base_color_tex != 0，需 kUV）或常值；
// metallic/roughness/emissive 取常值（纹理由后续阶段接入，见对应 *_tex 标志）。
// 当 object_use_default_color == true 时走纯色路径，颜色取 material.base_color。
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

    // Object3D 纯色开关（默认 false）。
    // 为 true 时所有 Object3D 走纯色渲染路径（kVs3d/kFs3d），忽略光照和 normal，
    // 颜色取 material.base_color（用于渲染无 kNormal 属性的网格）。
    // 为 false 时使用 GGX PBR 光照着色（需 mesh 含 kNormal）。
    bool object_use_default_color = false;

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
    // 着色行为取决于 RenderCommandList::object_use_default_color：
    //   - false（默认）：使用 GGX PBR 光照着色（需 mesh 含 kNormal）
    //   - true：使用旧的纯色渲染路径，material.base_color 作为物体颜色
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
                      const Vec3f& up, const Vec3f& front);
};

}  // namespace jpov

#endif  // JPOV_RENDER_COMMAND_H_
