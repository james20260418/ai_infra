// JPOV RenderCommandList — 帧级渲染指令输出
//
// OneIteration::Step() 产出 RenderCommandList，描述本帧要绘制的所有内容。
// 渲染后端消费这组指令将其画到屏幕上。
//
// 设计原则：
// - 流式绘制：每帧从零构建，无跨帧资源（顶点/颜色/文本都是字面量）
// - 指令列表 = 有序的绘制操作序列，按画家算法：先 3D 后 2D
// - 无 GPU 抽象：指令描述"画什么"，不描述"怎么画"
// - 全栈分配，无堆分配（可用 arena 或 std::vector，当前用 vector）
//
// 坐标系统一约定：
//   - 2D：屏幕像素坐标，原点在窗口左上角（x→右，y→下）
//   - 3D：世界空间，右手系（x→右，y→上，z→后）
//   - 2D 文本：基线对齐，位置即文本左下角坐标

#ifndef JPOV_RENDER_COMMAND_H_
#define JPOV_RENDER_COMMAND_H_

#include <cstdint>
#include <string>
#include <vector>

#include <glog/logging.h>

namespace jpov {

// ==================== 基础类型 ====================

struct Vec2 {
    float x, y;
};

struct Vec3 {
    float x, y, z;
};

// RGBA 颜色，分量 [0, 1]
struct Color {
    float r, g, b, a;

    // 常用颜色常量
    static const Color Red;
    static const Color Green;
    static const Color Blue;
    static const Color White;
    static const Color Black;
    static const Color Transparent;
};

// ==================== 绘制指令类型 ====================

// 渲染命令类型枚举
//
// Stream2D — 屏幕空间 2D 图元（线/矩形/圆/文本），由顶点列表描述
// Stream3D — 世界空间 3D 图元（线/三角形/包围盒/文本），由顶点列表描述
//
// MVP 阶段只输出这两种流式指令。未来扩展见底部注释。
enum class DrawCommandType : uint8_t {
    Stream2D = 0,
    Stream3D = 1,
};

// 顶点格式（MVP 阶段：位置 + 颜色）
//
// position — 位置坐标（2D：像素坐标栈；3D：世界空间坐标）
// color    — 顶点颜色（RGBA）
// uv       — 纹理坐标（保留，MVP 阶段不启用）
struct Vertex {
    Vec2 position;
    Color color;

    // Pre-condition: uv_x/uv_y 在 MVP 阶段无意义，调用者应设为 0
    float uv_x = 0.0f;
    float uv_y = 0.0f;
};

// ==================== 图元类型 ====================

// 图元拓扑（映射到 glDrawArrays 的 mode 参数）
//
// Lines        — 每两个顶点构成一条独立线段
// LineStrip    — 连续线段（顶点 1-2, 2-3, 3-4, ...）
// Triangles    — 每三个顶点构成一个独立三角形
// TriangleStrip — 连续三角带
//
// MVP 阶段默认使用 Lines 和 Triangles。
enum class PrimitiveTopology : uint8_t {
    Lines         = 0,
    LineStrip     = 1,
    Triangles     = 2,
    TriangleStrip = 3,
};

// ==================== 绘制指令 ====================

// 一条渲染命令
//
// type       — Stream2D 或 Stream3D
// topology   — 图元拓扑（Lines / Triangles 等）
// vertices   — 顶点列表
// debug_label — 语义标签，供 AI 校验使用（可选，默认为空）
struct DrawCommand {
    DrawCommandType type;
    PrimitiveTopology topology;
    std::vector<Vertex> vertices;
    std::string debug_label;
};

// ==================== 渲染指令列表 ====================

// 帧级输出：有序的绘制指令列表
//
// 渲染顺序：
//   1. 所有 Stream3D 指令（按添加顺序，由深度测试自动处理遮挡）
//   2. 所有 Stream2D 指令（按添加顺序，后画覆先画）
//
// 用户通过辅助方法添加指令，无需直接操作 commands vector。
struct RenderCommandList {
    std::vector<DrawCommand> commands;

    // 清空本帧所有指令（框架在每帧开始时调用）
    void Clear();

    // ---- 2D 绘制（屏幕空间，像素坐标） ----

    // 2D 线段
    //
    // Pre-condition: width > 0
    void DrawLine(Vec2 p1, Vec2 p2, Color color, float width = 1.0f,
                  const std::string& debug_label = "");

    // 2D 矩形
    //
    // pos — 矩形左上角（像素坐标）
    // size — 矩形宽高
    // filled — true=实心，false=空心
    void DrawRect(Vec2 pos, Vec2 size, Color color, bool filled = true,
                  const std::string& debug_label = "");

    // 2D 圆形
    //
    // center — 圆心坐标
    // radius — 半径（像素单位）
    // filled — true=实心，false=空心
    //
    // Pre-condition: radius > 0
    void DrawCircle(Vec2 center, float radius, Color color, bool filled = true,
                    const std::string& debug_label = "");

    // 2D 文本
    //
    // str — 文本内容
    // pos — 文本左下角基线坐标
    // font_size — 字号（像素单位）
    //
    // Pre-condition: font_size > 0
    void DrawText(const std::string& str, Vec2 pos, float font_size,
                  Color color, const std::string& debug_label = "");

    // ---- 3D 绘制（世界空间，右手系） ----

    // 3D 线段
    //
    // Pre-condition: width > 0
    void DrawLine3D(Vec3 p1, Vec3 p2, Color color, float width = 1.0f,
                    const std::string& debug_label = "");

    // 3D 三角形
    //
    // filled — true=实心，false=空心
    void DrawTriangle3D(Vec3 p1, Vec3 p2, Vec3 p3, Color color,
                        bool filled = true,
                        const std::string& debug_label = "");

    // 3D 包围盒线框
    //
    // min — 包围盒最小坐标
    // max — 包围盒最大坐标
    //
    // Pre-condition: min.x < max.x && min.y < max.y && min.z < max.z
    void DrawWireBox(Vec3 min, Vec3 max, Color color,
                     const std::string& debug_label = "");

    // 3D 文本（面向摄像机的标签）
    //
    // str — 文本内容
    // pos — 世界空间位置
    // font_size — 字号（像素单位）
    //
    // Pre-condition: font_size > 0
    void DrawText3D(const std::string& str, Vec3 pos, float font_size,
                    Color color, const std::string& debug_label = "");
};

// ==================== 未来扩展类型（注释） ====================
//
// 以下类型预留接口但不在 MVP 阶段实现：
//
// enum class DrawCommandType : uint8_t {
//     Stream2D,       // MVP
//     Stream3D,       // MVP
//     // 以下为未来：
//     StaticMesh,     // 常驻 mesh：mesh_id + transform
//     ImGuiBatch,     // ImGui 顶点/索引/纹理
//     ParticleSystem, // 粒子：base_mesh + instance data
// };
//

}  // namespace jpov

#endif  // JPOV_RENDER_COMMAND_H_
