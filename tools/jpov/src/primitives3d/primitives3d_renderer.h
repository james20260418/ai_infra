// JPOV Primitives3DRenderer — 3D 图元渲染（三角形/条带/线段/文本）
//
// 从 Renderer 拆分出来的静态工具集：负责 3D 纯色/纹理图元的绘制、
// MVP 矩阵构建（纯 CPU，不碰 GL 矩阵栈）以及对应的 GLSL shader 源码。
//
// 与 FontRenderer/Object3DRenderer 保持一致：所有方法均为 static，
// 不含实例状态；GL 资源（stream_vbo、strip_vbo、fbo 尺寸等）由 Renderer
// 持有并通过参数传入。

#ifndef JPOV_PRIMITIVES3D_RENDERER_H_
#define JPOV_PRIMITIVES3D_RENDERER_H_

#include <vector>

#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/interface/camera.h"

namespace jpov {

class Primitives3DRenderer {
public:
    // ---- 3D GLSL shader 源码 ----
    //
    // 调用方用 ShaderManager 编译：
    //   shader_mgr.GetOrCreate("solid3d", {kVs3d, kFs3d});
    //   shader_mgr.GetOrCreate("text3d", {kTexVs3d, kTexFs});
    //
    // kVs3d: 3D 顶点 shader，接受 vec3 世界坐标，通过 MVP 变换到 NDC（纯色）
    static constexpr const char* kVs3d = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uMVP;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)glsl";

    // kFs3d: 3D 纯色 fragment shader
    static constexpr const char* kFs3d = R"glsl(
#version 330 core
out vec4 FragColor;
uniform vec4 uColor;

void main() {
    FragColor = uColor;
}
)glsl";

    // kTexVs3d: 3D 纹理顶点 shader（用于 Text3D）
    static constexpr const char* kTexVs3d = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexCoord;
uniform mat4 uMVP;
out vec2 vTexCoord;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vTexCoord = aTexCoord;
}
)glsl";

    // kText3dFs: 3D 文本 fragment shader（无光照纯色）。
    //
    // 与 2D 的 kTextFs 同构：从单通道（GL_R8）字形 atlas 读 alpha，乘 uniform 颜色。
    // 区别只在顶点侧（kTexVs3d 走 MVP 世界→NDC，而非 2D 的像素→NDC），
    // 所以这里能**直接复用** 2D 的 fragment 逻辑——3D 文本「无光照纯色」的语义
    // 就是「不参与 PBR，直接把字形 alpha 当覆盖率输出颜色」。
    static constexpr const char* kText3dFs = R"glsl(
#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;
uniform sampler2D uTexture;
uniform vec4 uColor;
void main() {
    float alpha = texture(uTexture, vTexCoord).r;
    FragColor = vec4(uColor.rgb, uColor.a * alpha);
}
)glsl";

    // ---- MVP / Model 矩阵构建（纯 CPU，不碰 GL 矩阵栈）----

    // 4x4 矩阵乘法：out = a * b（列主序）
    static void Mat4Mul(const float a[16], const float b[16], float out[16]);

    // 列主序透视投影矩阵（对应 GLM::perspective / glFrustum 语义）
    // 构建右手系透视投影：fov_y, aspect, near, far
    static void BuildPerspProj(float fov_y, float aspect, float near, float far,
                               float out[16]);

    // 从 Camera 构建 MVP 矩阵（纯 CPU，不碰 GL 矩阵栈，不读回 GL 状态）
    // MVP = Projection * View
    static void BuildMVP(const Camera& cam, int fbo_w, int fbo_h, float mvp[16]);

    // 从 center/up/front 构建 Model 矩阵（列主序，纯 CPU，不碰 GL 矩阵栈）。
    // scale 为整体缩放（先缩放顶点，再 up/front 旋转 + center 平移，见 BuildModelMatrix 实现）。
    static void BuildModelMatrix(const Vec3f& center, const Vec3f& up,
                                 const Vec3f& front, float model[16],
                                 float scale = 1.0f);

    // ---- 3D 图元绘制 ----
    //
    // 全部 static，通过参数传入共享 GL 资源：
    //   stream_vbo / strip_vbo: Renderer 持有的 VBO
    //   prog: 已编译的 shader program（Solid3DProg / Text3DProg）
    //   mvp: 当前 Camera 的 MVP 矩阵
    //   fbo_w / fbo_h: 3D FBO 尺寸
    static void DrawTriangle3D(const Triangle3DCommand& cmd,
                               unsigned int stream_vbo, unsigned int prog,
                               const float mvp[16]);
    static void DrawStrip3D(const Strip3DCommand& cmd,
                            unsigned int strip_vbo, unsigned int prog,
                            const float mvp[16]);
    static void DrawLine3D(const Line3DCommand& cmd,
                           unsigned int stream_vbo, unsigned int prog,
                           const float mvp[16]);

    // ---- 3D 文本几何生成（纯 CPU，GL-free，可单测）----
    //
    // 把一段文本按 (anchor, face_direction, up_direction) 摆放到世界空间的
    // 一个平面 quad 上。字形排版 / 包围盒 / 对齐偏移完全复用 FontManager 的
    // GenerateTextVertices()（与 2D 同源），本函数只负责把「文本平面局部坐标」
    // 映射到世界坐标。
    //
    // 参数：
    //   cmd:            命令（含 anchor / face / up / alignment）
    //   font_height_px: 本次排版的目标**像素高度**（= FontManager 语义的 font_size）。
    //                   由调用方从 cmd.font_height_world 换算而来（见 scale_out）。
    //   glyph_verts:    in/out — FontManager 产出的字形顶点（每顶点 4 floats：
    //                   x,y,u,v，x/y 是文本平面局部像素坐标，原点在不施对齐的
    //                   左上演进阶起点），会被就地转换成世界坐标。
    //
    // 副作用：
    //   - 把 glyph_verts 就地转化成世界坐标（x,y → 世界 xyz；u,v 不变）。
    //     转换后元素布局变为每顶点 5 floats：x,y,z,u,v。
    //   - scale_out: 排版像素 → 世界米的统一缩放因子（= height_world / 像素高）。
    //
    // 实现（与 Object3D 的 up/front 处理同一套正交化）：
    //   n = normalize(face)                            // quad 法线
    //   u = normalize(up - dot(up,n)*n)                // 文字上（Gram-Schmidt）
    //   r = cross(u, n)                                // 文字右
    //   world = anchor + r*(lx*scale) - u*(ly*scale)
    //     注意 ly 取负：文本平面 y 轴向下（与 2D 屏幕同向），而 u 向上。
    //
    // Pre-condition: cmd.face_direction / cmd.up_direction 非零且不共线
    static void BuildText3DWorldVerts(const Text3DCommand& cmd,
                                      float font_height_px,
                                      std::vector<float>* glyph_verts,
                                      float* scale_out);
};

}  // namespace jpov

#endif
