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
    static void DrawText3D(const Text3DCommand& cmd,
                           unsigned int stream_vbo, unsigned int prog,
                           const float mvp[16], int fbo_w, int fbo_h);
};

}  // namespace jpov

#endif
