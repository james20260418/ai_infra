// JPOV Renderer 实现
// FBO 动态调整，坐标以窗口坐标为空间。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <tuple>
#include <vector>

#include "geom/common/common.h"

#include <GLFW/glfw3.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include <glog/logging.h>

// stb_image_write — 轻量级 PNG 编码
#include "stb_image_write.h"

// Windows/MinGW: use function pointers loaded at runtime via wglGetProcAddress
// Linux/Mesa: use standard GL symbols (exported directly by libGL)
//
// 注意：MinGW 的 GL 库不导出 renderbuffer / MSAA 的 GL 3.x 核心函数，
// 因此 Windows 下 3D FBO 不使用 MSAA（带 #define JPOV_WITHOUT_MSAA）。
#ifdef _WIN32
#define JPOV_WITHOUT_MSAA
#include "third_party/gl_loader-mingw/gl_loader.h"
// glXxx→gl_Xxx 别名宏已集成在 gl_loader.h 中，本文件不再重复定义。

// MinGW 的 GL/gl.h 可能不定义 GL_CLAMP_TO_EDGE
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#endif

namespace {

using jpov::Vec3f;

// 窗口坐标 → NDC 标准化设备坐标
// 原点在窗口左上角，x→右，y→下
// 2D 坐标使用窗口尺寸做 NDC 变换（坐标超出 FBO 范围即裁剪）
const char* kVs = R"glsl(
#version 330 core
layout(location = 0) in vec2 aPos;
uniform vec2 uFboSize;

void main() {
    vec2 ndc = (aPos / uFboSize) * 2.0 - 1.0;
    ndc.y = -ndc.y;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
)glsl";

const char* kFs = R"glsl(
#version 330 core
out vec4 FragColor;
uniform vec4 uColor;

void main() {
    FragColor = uColor;
}
)glsl";

// ==================== 3D Shaders ====================

// 3D 顶点 shader：接受 vec3 世界坐标，通过 MVP 矩阵变换到 NDC
const char* kVs3d = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uMVP;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)glsl";

// 3D Fragment Shader（纯色）
const char* kFs3d = R"glsl(
#version 330 core
out vec4 FragColor;
uniform vec4 uColor;

void main() {
    FragColor = uColor;
}
)glsl";

// 3D 纹理顶点 shader（用于 Text3D）
const char* kTexVs3d = R"glsl(
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

// ==================== 3D 静态模型 Shaders（用于 Object3D）====================

// 3D 静态模型顶点 shader：接受局部空间位置 + UV，
// 通过 MVP（含 Model 变换）映射到 NDC。
// 属性 location 与 MeshManager 的 VBO 布局一致：
//   0 = position，1 = normal，2 = uv
const char* kMeshVs3d = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aTexCoord;
uniform mat4 uMVP;
out vec2 vTexCoord;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vTexCoord = aTexCoord;
}
)glsl";

// 3D 静态模型纹理 Fragment Shader：纹理×tint 混合（RGB 采样）
const char* kMeshTexFs = R"glsl(
#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;
uniform sampler2D uTexture;
uniform vec4 uTint;

void main() {
    vec4 tex_color = texture(uTexture, vTexCoord);
    FragColor = tex_color * uTint;
}
)glsl";

// ==================== 3D 光照 Shaders（Tiled Forward Blinn-Phong）====================

// 3D 静态模型光照顶点 shader：
// 接受局部空间 position + normal，输出 world-space position + normal + gl_Position
// 属性 location 与 MeshManager 的 VBO 布局一致：
//   0 = position，1 = normal
const char* kMeshVs3dLighting = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
uniform mat4 uMVP;
uniform mat4 uModel;
out vec3 vWorldPos;
out vec3 vWorldNormal;

void main() {
    vec4 world_pos = uModel * vec4(aPos, 1.0);
    vWorldPos = world_pos.xyz;
    // normal matrix = inverse(transpose(mat3(uModel)))，支持非均匀缩放
    vWorldNormal = normalize(mat3(transpose(inverse(uModel))) * aNormal);
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)glsl";

// Tiled Forward 光照 fragment shader
//
// 数据流：
//   CPU 端：每个点光源投影到 NDC → screen rect → 覆盖的 tile(s) 写入 light index
//   GPU 端：fragment 根据 gl_FragCoord 查 tile 纹理获取 ≤16 个光源 index，
//          遍历这些光源做 Blinn-Phong 计算（动态索引 uLights 数组）
//
// tile 纹理：普通 GL_RGBA8（避开 integer 纹理的兼容坑），每 texel 的 RGBA 通道
//           各存一个 uint8 光源 index（255 = 无光源哨兵），
//           一个 tile 16 个 index = 4 个相邻 texel（水平排列）。
// tile 网格：ceil(fbo_w/16) × ceil(fbo_h/16)，纹理宽 = grid_cols*4
//
// 光源数据本身通过 flat uniform 数组传递（上限 MAX_TOTAL_LIGHTS）。
// tile 纹理里的 index 是光源在 point_lights / uLights 数组中的下标，
// fragment 用它做动态索引，只计算本 tile 命中的光源。
//
// !!! 这些 #define 必须与 renderer.h 的 C++ 常量保持一致：
//     TILE_SIZE        ↔ kTileSize16_       (=16)
//     MAX_LIGHTS_PER_TILE ↔ kMaxLightsPerTile_ (=16)
//     MAX_TOTAL_LIGHTS ↔ kMaxTotalLights_   (=255)
//     LIGHT_INDEX_SENTINEL ↔ kLightIndexSentinel_ (=255)
//  改任一处必须同时改另一处，否则 tile 编码/解码错位。
const char* kMeshFs3dTiledLighting = R"glsl(
#version 330 core

#define TILE_SIZE 16
#define MAX_LIGHTS_PER_TILE 16
#define MAX_TOTAL_LIGHTS 255
#define LIGHT_INDEX_SENTINEL 255u

in vec3 vWorldPos;
in vec3 vWorldNormal;
out vec4 FragColor;

// 点光源
struct Light {
    vec3 position;
    vec3 color;
    float radius;
};

uniform Light uLights[MAX_TOTAL_LIGHTS];
uniform int uTotalLights;
uniform vec3 uCameraPos;
uniform vec3 uModelColor;
uniform float uShininess;

// Tile culling 纹理：GL_RGBA8，每 texel 的 4 通道各存一个 uint8 光源 index
// （255 = 无光源）。fragment shader 用 texelFetch 精准定位。
uniform sampler2D uTileLightIndices;

// 环境光常量
const vec3 AMBIENT_COLOR = vec3(0.05, 0.05, 0.08);
const float AMBIENT_STRENGTH = 0.3;

// 从 tile 纹理读取当前 tile 的光源 index 列表
// tile_col/row: 当前像素所在的 tile 坐标
// out_indices: 输出的 16 个光源 index（未使用的填 LIGHT_INDEX_SENTINEL=255）
// returns: 实际有效光源数
int readTileLights(int tile_col, int tile_row, out uint out_indices[MAX_LIGHTS_PER_TILE]) {
    int count = 0;
    // 每 tile 存 (MAX_LIGHTS_PER_TILE/4) 个 texel（水平排列），
    // 每 texel 的 RGBA 各存 1 个 index；4 = RGBA 颜色通道数
    for (int t = 0; t < MAX_LIGHTS_PER_TILE / 4; t++) {
        vec4 px = texelFetch(uTileLightIndices, ivec2(tile_col * (MAX_LIGHTS_PER_TILE / 4) + t, tile_row), 0);
        // 编码：通道值 255 表示该通道无光源；其余 0-254 为光源 index
        //（uint8 → uint，再 & 0xFF 防浮点取整误差）
        uint r = uint(px.r * 255.0 + 0.5) & 0xFFu;
        uint g = uint(px.g * 255.0 + 0.5) & 0xFFu;
        uint b = uint(px.b * 255.0 + 0.5) & 0xFFu;
        uint a = uint(px.a * 255.0 + 0.5) & 0xFFu;
        if (r != LIGHT_INDEX_SENTINEL && r < uint(uTotalLights) && count < MAX_LIGHTS_PER_TILE) {
            out_indices[count] = r;
            count++;
        }
        if (g != LIGHT_INDEX_SENTINEL && g < uint(uTotalLights) && count < MAX_LIGHTS_PER_TILE) {
            out_indices[count] = g;
            count++;
        }
        if (b != LIGHT_INDEX_SENTINEL && b < uint(uTotalLights) && count < MAX_LIGHTS_PER_TILE) {
            out_indices[count] = b;
            count++;
        }
        if (a != LIGHT_INDEX_SENTINEL && a < uint(uTotalLights) && count < MAX_LIGHTS_PER_TILE) {
            out_indices[count] = a;
            count++;
        }
    }
    return count;
}

void main() {
    vec3 N = normalize(vWorldNormal);
    vec3 V = normalize(uCameraPos - vWorldPos);

    // 计算当前像素所在的 tile，并 clamp 到合法范围（防御边缘/亚像素越界）：
    //   grid_cols = textureWidth / kTexelsPerTile(=4)，grid_rows = textureHeight
    ivec2 grid = ivec2(textureSize(uTileLightIndices, 0));
    int grid_cols = grid.x / (MAX_LIGHTS_PER_TILE / 4);
    int grid_rows = grid.y;
    int tile_col = clamp(int(gl_FragCoord.x) / TILE_SIZE, 0, grid_cols - 1);
    int tile_row = clamp(int(gl_FragCoord.y) / TILE_SIZE, 0, grid_rows - 1);

    // 从 tile 纹理读取本 tile 的光源列表
    uint light_indices[MAX_LIGHTS_PER_TILE];
    int num_lights = readTileLights(tile_col, tile_row, light_indices);

    vec3 ambient = AMBIENT_COLOR * AMBIENT_STRENGTH;
    vec3 total_diffuse = vec3(0.0);
    vec3 total_specular = vec3(0.0);

    for (int i = 0; i < num_lights; i++) {
        uint li = light_indices[i];
        vec3 L = uLights[li].position - vWorldPos;
        float dist = length(L);
        if (dist >= uLights[li].radius) continue;

        vec3 light_dir = L / dist;

        // 线性衰减：1.0 at dist=0 → 0.0 at dist=radius
        float attenuation = 1.0 - (dist / uLights[li].radius);

        // Diffuse (Lambert)
        float NdotL = max(dot(N, light_dir), 0.0);
        total_diffuse += uLights[li].color * NdotL * attenuation;

        // Specular (Blinn-Phong)
        vec3 H = normalize(light_dir + V);
        float NdotH = max(dot(N, H), 0.0);
        total_specular += uLights[li].color * pow(NdotH, uShininess) * attenuation;
    }

    vec3 result = uModelColor * (ambient + total_diffuse) + total_specular;
    FragColor = vec4(result, 1.0);
}
)glsl";

// ==================== MVP 矩阵构建（纯 CPU，不碰 GL 矩阵栈）====================

// 4x4 矩阵乘法：out = a * b（列主序）
static void Mat4Mul(const float a[16], const float b[16], float out[16]) {
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a[k * 4 + row] * b[col * 4 + k];
            }
            out[col * 4 + row] = sum;
        }
    }
}

// 列主序透视投影矩阵（对应 GLM::perspective / glFrustum 语义）
// 构建右手系透视投影：fov_y, aspect, near, far
static void BuildPerspProj(float fov_y, float aspect, float near, float far,
                            float out[16]) {
    float f = 1.0f / std::tan(fov_y * 0.5f);
    float range_inv = 1.0f / (near - far);
    // 列主序
    out[0]  = f / aspect;  out[4]  = 0.0f; out[8]  = 0.0f;                 out[12] = 0.0f;
    out[1]  = 0.0f;        out[5]  = f;     out[9]  = 0.0f;                 out[13] = 0.0f;
    out[2]  = 0.0f;        out[6]  = 0.0f;  out[10] = (near + far) * range_inv; out[14] = 2.0f * near * far * range_inv;
    out[3]  = 0.0f;        out[7]  = 0.0f;  out[11] = -1.0f;                out[15] = 0.0f;
}

// 从 Camera 构建 MVP 矩阵（纯 CPU，不碰 GL 矩阵栈，不读回 GL 状态）
// MVP = Projection * View
// Projection: 透视投影（列主序）
// View: lookAt 矩阵（列主序）
static void BuildMVP(const jpov::Camera& cam, int fbo_w, int fbo_h, float mvp[16]) {
    float aspect = static_cast<float>(fbo_w) / static_cast<float>(fbo_h);
    float fov_rad = cam.fov * 3.14159265358979323846f / 180.0f;

    // === 投影矩阵 ===
    float proj[16];
    BuildPerspProj(fov_rad, aspect, cam.near, cam.far, proj);

    // === lookAt 视图矩阵 ===
    // 计算坐标基
    Vec3f fwd = cam.target - cam.position;
    float f_len = std::sqrt(fwd.x()*fwd.x() + fwd.y()*fwd.y() + fwd.z()*fwd.z());
    if (f_len < 1e-8f) { fwd = {0.0f, 0.0f, -1.0f}; }
    else { fwd = {fwd.x()/f_len, fwd.y()/f_len, fwd.z()/f_len}; }

    Vec3f side = {fwd.y()*cam.up.z() - fwd.z()*cam.up.y(),
                  fwd.z()*cam.up.x() - fwd.x()*cam.up.z(),
                  fwd.x()*cam.up.y() - fwd.y()*cam.up.x()};
    float s_len = std::sqrt(side.x()*side.x() + side.y()*side.y() + side.z()*side.z());
    if (s_len < 1e-8f) { side = {1.0f, 0.0f, 0.0f}; }
    else { side = {side.x()/s_len, side.y()/s_len, side.z()/s_len}; }

    Vec3f upv = {side.y()*fwd.z() - side.z()*fwd.y(),
                 side.z()*fwd.x() - side.x()*fwd.z(),
                 side.x()*fwd.y() - side.y()*fwd.x()};

    // 列主序 lookAt 矩阵 (OpenGL 右手系)
    float view[16] = {
        side.x(), upv.x(), -fwd.x(), 0.0f,
        side.y(), upv.y(), -fwd.y(), 0.0f,
        side.z(), upv.z(), -fwd.z(), 0.0f,
        -(side.x()*cam.position.x() + side.y()*cam.position.y() + side.z()*cam.position.z()),
        -(upv.x()*cam.position.x() + upv.y()*cam.position.y() + upv.z()*cam.position.z()),
         (fwd.x()*cam.position.x() + fwd.y()*cam.position.y() + fwd.z()*cam.position.z()),
         1.0f
    };

    // MVP = Proj * View
    Mat4Mul(proj, view, mvp);
}

// 从 center/up/front 构建 Model 矩阵（列主序，纯 CPU，不碰 GL 矩阵栈）
//
// 把局部空间（mesh 定义坐标）映射到世界空间：
//   - 局部 +Y → 世界 up
//   - 局部 +Z → 世界 front
//   - 局部 +X → normalize(cross(up, front))（保证右手系）
// 再平移 center。无缩放。
//
// Model = T(center) * R(right, up, front)
// Pre-condition: up、front 均非零且不平行（调用方 DrawObject3D 已校验）
static void BuildModelMatrix(const jpov::Vec3f& center,
                             const jpov::Vec3f& up,
                             const jpov::Vec3f& front,
                             float model[16]) {
    // 归一化 up 与 front
    float u_len = std::sqrt(up.x()*up.x() + up.y()*up.y() + up.z()*up.z());
    float f_len = std::sqrt(front.x()*front.x() + front.y()*front.y() + front.z()*front.z());
    jpov::Vec3f upn = {up.x()/u_len, up.y()/u_len, up.z()/u_len};
    jpov::Vec3f frn = {front.x()/f_len, front.y()/f_len, front.z()/f_len};

    // right = normalize(cross(up, front))——保证模型右手系
    jpov::Vec3f right = {upn.y()*frn.z() - upn.z()*frn.y(),
                         upn.z()*frn.x() - upn.x()*frn.z(),
                         upn.x()*frn.y() - upn.y()*frn.x()};
    float r_len = std::sqrt(right.x()*right.x() + right.y()*right.y() + right.z()*right.z());
    right = {right.x()/r_len, right.y()/r_len, right.z()/r_len};

    // 列主序旋转部分：
    //   model[0..2] = right 列（世界 X 轴方向）
    //   model[4..6] = upn   列（世界 Y 轴方向）
    //   model[8..10]= frn   列（世界 Z 轴方向）
    // 平移在最后一列（12..14）
    model[0] = right.x(); model[4] = upn.x(); model[8]  = frn.x(); model[12] = center.x();
    model[1] = right.y(); model[5] = upn.y(); model[9]  = frn.y(); model[13] = center.y();
    model[2] = right.z(); model[6] = upn.z(); model[10] = frn.z(); model[14] = center.z();
    model[3] = 0.0f;      model[7] = 0.0f;    model[11] = 0.0f;    model[15] = 1.0f;
}

// 纹理+颜色混合 Fragment Shader
// 纹理采样（alpha 通道作为透明度）× uniform 颜色
const char* kTexVs = R"glsl(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
uniform vec2 uFboSize;
out vec2 vTexCoord;

void main() {
    vec2 ndc = (aPos / uFboSize) * 2.0 - 1.0;
    ndc.y = -ndc.y;
    gl_Position = vec4(ndc, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)glsl";

const char* kTexFs = R"glsl(
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

// RGBA 纹理 Fragment Shader（用于 Image2D 全彩纹理）
const char* kImageFs = R"glsl(
#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;
uniform sampler2D uTexture;
uniform vec4 uTint;

void main() {
    vec4 tex_color = texture(uTexture, vTexCoord);
    FragColor = tex_color * uTint;
}
)glsl";

// 创建 GL atlas 纹理并上传 CPU 像素（初始全黑）
unsigned int CreateGlAtlasTexture(int atlas_dim,
                                  const std::vector<uint8_t>& pixels) {
    unsigned int tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    // 用 GL_R8 单通道纹理，shader 中 .r 读取为 alpha
    // 先传全黑数据占位
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, atlas_dim, atlas_dim, 0,
                 GL_RED, GL_UNSIGNED_BYTE, pixels.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

}  // anonymous namespace

namespace jpov {

const Color kColorRed         = {1.0f, 0.0f, 0.0f, 1.0f};
const Color kColorGreen       = {0.0f, 1.0f, 0.0f, 1.0f};
const Color kColorBlue        = {0.0f, 0.0f, 1.0f, 1.0f};
const Color kColorWhite       = {1.0f, 1.0f, 1.0f, 1.0f};
const Color kColorBlack       = {0.0f, 0.0f, 0.0f, 1.0f};
const Color kColorTransparent = {0.0f, 0.0f, 0.0f, 0.0f};

// === Renderer ===

Renderer::Renderer() = default;

Renderer::~Renderer() {
    DestroyFBO();
    DestroyOutputFBO();
    Destroy3DFBO();
    Destroy3DResolveFBO();
    DestroyTileLighting();
    // 注意：shader program 由 ShaderManager::~ShaderManager() 统一释放
    if (stream_vbo_)   glDeleteBuffers(1, &stream_vbo_);
    if (strip_vbo_)    glDeleteBuffers(1, &strip_vbo_);
    // Font GL textures (所有注册字体的三层 atlas)
    for (auto& [alias, slot] : font_slots_) {
        (void)alias;
        for (int lv = 0; lv < 3; ++lv) {
            if (slot.atlas_tex[lv]) glDeleteTextures(1, &slot.atlas_tex[lv]);
        }
    }
}

void Renderer::DestroyFBO() {
    if (fbo_) {
        glDeleteFramebuffers(1, &fbo_);
        glDeleteTextures(1, &color_tex_);
        fbo_ = 0;
        color_tex_ = 0;
    }
    fbo_w_ = 0;
    fbo_h_ = 0;
}

void Renderer::DestroyOutputFBO() {
    if (out_fbo_) {
        glDeleteFramebuffers(1, &out_fbo_);
        glDeleteTextures(1, &out_color_tex_);
        out_fbo_ = 0;
        out_color_tex_ = 0;
    }
    out_w_ = 0;
    out_h_ = 0;
}

void Renderer::Destroy3DFBO() {
    if (fbo_3d_) {
        glDeleteFramebuffers(1, &fbo_3d_);
        glDeleteTextures(1, &color_tex_3d_);
#ifndef JPOV_WITHOUT_MSAA
        if (depth_rb_3d_) {
            glDeleteRenderbuffers(1, &depth_rb_3d_);
        }
#else
        if (depth_tex_3d_) {
            glDeleteTextures(1, &depth_tex_3d_);
        }
#endif
        fbo_3d_ = 0;
        color_tex_3d_ = 0;
        depth_rb_3d_ = 0;
        depth_tex_3d_ = 0;
    }
    fbo_3d_w_ = 0;
    fbo_3d_h_ = 0;
}

void Renderer::Destroy3DResolveFBO() {
    if (resolve_fbo_3d_) {
        glDeleteFramebuffers(1, &resolve_fbo_3d_);
        glDeleteTextures(1, &resolve_tex_3d_);
        resolve_fbo_3d_ = 0;
        resolve_tex_3d_ = 0;
    }
    resolve_fbo_3d_w_ = 0;
    resolve_fbo_3d_h_ = 0;
}

void Renderer::DestroyTileLighting() {
    if (tile_index_tex_) {
        glDeleteTextures(1, &tile_index_tex_);
        tile_index_tex_ = 0;
    }
    tile_grid_w_ = 0;
    tile_grid_h_ = 0;
}

void Renderer::EnsureTileLighting(int fbo_w_3d, int fbo_h_3d) {
    // 计算 tile 网格尺寸
    int grid_cols = (fbo_w_3d + kTileSize16_ - 1) / kTileSize16_;
    int grid_rows = (fbo_h_3d + kTileSize16_ - 1) / kTileSize16_;

    // 纹理宽度 = grid_cols * 4（每个 tile 水平排 4 个 RGBA texel，每 texel 的
    // RGBA 通道各存 1 个 uint8 index，4 texel = 16 个 index）
    int tex_w = grid_cols * kTexelsPerTile_;
    int tex_h = grid_rows;

    bool need_create =
        tile_index_tex_ == 0 || tile_grid_w_ != grid_cols || tile_grid_h_ != grid_rows;

    if (need_create) {
        if (tile_index_tex_) {
            glDeleteTextures(1, &tile_index_tex_);
        }
        glGenTextures(1, &tile_index_tex_);
        glBindTexture(GL_TEXTURE_2D, tile_index_tex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex_w, tex_h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        tile_grid_w_ = grid_cols;
        tile_grid_h_ = grid_rows;
    }
}

void Renderer::EnsureFBO(int width, int height) {
    if (fbo_w_ == width && fbo_h_ == height && fbo_) return;

    CHECK_GT(width, 0);
    CHECK_GT(height, 0);
    CHECK_LE(width, kMaxFboDim);
    CHECK_LE(height, kMaxFboDim);

    DestroyFBO();

    glGenTextures(1, &color_tex_);
    glBindTexture(GL_TEXTURE_2D, color_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, color_tex_, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    CHECK_EQ(status, GL_FRAMEBUFFER_COMPLETE)
        << "FBO failed, status=" << status;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    fbo_w_ = width;
    fbo_h_ = height;
}

void Renderer::Ensure3DFBO(int width, int height) {
    if (fbo_3d_w_ == width && fbo_3d_h_ == height && fbo_3d_) return;

    CHECK_GT(width, 0);
    CHECK_GT(height, 0);
    CHECK_LE(width, kMaxFboDim);
    CHECK_LE(height, kMaxFboDim);

    Destroy3DFBO();
    Destroy3DResolveFBO();
    DestroyTileLighting();

#ifdef JPOV_WITHOUT_MSAA
    // 非 MSAA 路径（Windows/MinGW 下 GL 不导出 MSAA 函数）
    // 直接用普通 2D 纹理 + 深度纹理
    glGenTextures(1, &color_tex_3d_);
    glBindTexture(GL_TEXTURE_2D, color_tex_3d_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenTextures(1, &depth_tex_3d_);
    glBindTexture(GL_TEXTURE_2D, depth_tex_3d_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height, 0,
                 GL_DEPTH_COMPONENT, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &fbo_3d_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_3d_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, color_tex_3d_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                           GL_TEXTURE_2D, depth_tex_3d_, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    CHECK_EQ(status, GL_FRAMEBUFFER_COMPLETE)
        << "3D FBO (non-MSAA) failed, status=" << status;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // 非 MSAA 路径不需要 separate resolve FBO（直接用 fbo_3d_ blit）
    resolve_fbo_3d_ = 0;
    resolve_tex_3d_ = 0;
    resolve_fbo_3d_w_ = 0;
    resolve_fbo_3d_h_ = 0;
#else
    // === 4x MSAA 颜色纹理 ===
    glGenTextures(1, &color_tex_3d_);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, color_tex_3d_);
    glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, 4, GL_RGBA8,
                            width, height, GL_TRUE);
    glTexParameteri(GL_TEXTURE_2D_MULTISAMPLE, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_MULTISAMPLE, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);

    // === 深度 renderbuffer（MSAA 需要） ===
    glGenRenderbuffers(1, &depth_rb_3d_);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_rb_3d_);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_DEPTH_COMPONENT24,
                                     width, height);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    // === FBO 对象 ===
    glGenFramebuffers(1, &fbo_3d_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_3d_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D_MULTISAMPLE, color_tex_3d_, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, depth_rb_3d_);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    CHECK_EQ(status, GL_FRAMEBUFFER_COMPLETE)
        << "3D MSAA FBO failed, status=" << status;

    // === 中间 non-MSAA resolve FBO（同尺寸，用于 resolve + 缩放 blit） ===
    glGenTextures(1, &resolve_tex_3d_);
    glBindTexture(GL_TEXTURE_2D, resolve_tex_3d_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &resolve_fbo_3d_);
    glBindFramebuffer(GL_FRAMEBUFFER, resolve_fbo_3d_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, resolve_tex_3d_, 0);

    status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    CHECK_EQ(status, GL_FRAMEBUFFER_COMPLETE)
        << "3D resolve FBO failed, status=" << status;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    resolve_fbo_3d_w_ = width;
    resolve_fbo_3d_h_ = height;
#endif

    fbo_3d_w_ = width;
    fbo_3d_h_ = height;
}

// ---- Shader program 访问器 ----
// 通过 ShaderManager 按名字获取。GetOrCreate 幂等：首次调用编译+缓存，
// 后续直接返回缓存的 program，因此可在 Init 和每次 draw 时安全调用。

unsigned int Renderer::SolidProg() {
    return shader_mgr_.GetOrCreate("solid", {kVs, kFs});
}

unsigned int Renderer::TextProg() {
    return shader_mgr_.GetOrCreate("text", {kTexVs, kTexFs});
}

unsigned int Renderer::ImageProg() {
    return shader_mgr_.GetOrCreate("image", {kTexVs, kImageFs});
}

unsigned int Renderer::Solid3DProg() {
    return shader_mgr_.GetOrCreate("solid3d", {kVs3d, kFs3d});
}

unsigned int Renderer::Text3DProg() {
    return shader_mgr_.GetOrCreate("text3d", {kTexVs3d, kTexFs});
}

unsigned int Renderer::TexturedMesh3DProg() {
    return shader_mgr_.GetOrCreate("mesh3d_textured", {kMeshVs3d, kMeshTexFs});
}

unsigned int Renderer::MeshLighting3DProg() {
    return shader_mgr_.GetOrCreate("mesh3d_tiled_lighting",
                                  {kMeshVs3dLighting, kMeshFs3dTiledLighting});
}

// 编译/注册全部 shader program。
// 通过 ShaderManager 统一管理（编译失败 → LOG(FATAL) crash）。
void Renderer::CompileShaders() {
    // 首次调用即触发编译+缓存；重复调用幂等。
    SolidProg();
    TextProg();
    ImageProg();
    Solid3DProg();
    Text3DProg();
    TexturedMesh3DProg();
    MeshLighting3DProg();
}

void Renderer::CreateStreamVBO() {
    size_t buf = static_cast<size_t>(kMaxStreamVertices) * 2 * sizeof(float);
    glGenBuffers(1, &stream_vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
    glBufferData(GL_ARRAY_BUFFER, buf, nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // Strip3D 专用 VBO：3000 顶点 × 3 floats × sizeof(float)
    size_t strip_buf = static_cast<size_t>(kMaxStripVertices) * 3 * sizeof(float);
    glGenBuffers(1, &strip_vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, strip_vbo_);
    glBufferData(GL_ARRAY_BUFFER, strip_buf, nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void Renderer::Init(
    const std::vector<std::tuple<const char*, int, const char*>>& font_entries,
    const std::vector<std::tuple<const char*, int, const char*>>& default_fonts) {
#ifdef _WIN32
    CHECK_EQ(gl_loader_init(), 0) << "Failed to load OpenGL 3.x functions";
#endif
    CompileShaders();
    CreateStreamVBO();
    InitFonts(font_entries, default_fonts);
}


// ==================== 字体初始化 ====================

// 路径查找：先试原始路径，再试 bazel test 的 runfiles（TEST_SRCDIR）
static std::string ResolveFontPath(const char* raw_path) {
    FILE* fp = std::fopen(raw_path, "rb");
    if (fp) {
        std::fclose(fp);
        return raw_path;
    }
    // Try TEST_SRCDIR for bazel test sandbox
    const char* srcdir = std::getenv("TEST_SRCDIR");
    if (srcdir) {
        std::string p = srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/";
        p += raw_path;
        FILE* fp2 = std::fopen(p.c_str(), "rb");
        if (fp2) {
            std::fclose(fp2);
            return p;
        }
    }
    return "";
}

// ==================== InitOneFontSlot ====================

void Renderer::InitOneFontSlot(const char* alias,
                                const std::string& resolved_path,
                                int ttc_index,
                                FontSlot* slot /*output*/) {
    CHECK(slot != nullptr);
    CHECK(!slot->manager.has_value()) << "FontSlot already initialized for alias=" << alias;

    FontManagerConfig cfg;
    cfg.font_name = alias;
    cfg.font_path = resolved_path;
    cfg.ttc_font_index = ttc_index;

    std::optional<FontManager> mgr = FontManager::Create(cfg);
    CHECK(mgr.has_value())
        << "Failed to load font: alias=" << alias
        << " path=" << resolved_path
        << " ttc_index=" << ttc_index;

    slot->manager = std::move(mgr.value());
    for (int lv = 0; lv < 3; ++lv) {
        slot->atlas_tex[lv] = CreateGlAtlasTexture(
            FontManager::kAtlasDim,
            slot->manager->atlas_pixels(lv));
    }

    LOG(INFO) << "Font registered: alias=" << alias
              << " path=" << resolved_path
              << " ttc_index=" << ttc_index;
}

// ==================== RegisterFont ====================

void Renderer::RegisterFont(const char* path,
                              int ttc_index,
                              const char* alias,
                              const char* source,
                              std::unordered_map<std::string, FontSlot>* font_slots,
                              std::vector<std::string>* font_order) {
    CHECK(path != nullptr && path[0] != '\0')
        << "FontEntry path is null or empty (alias=" << alias << ")";
    CHECK(alias != nullptr && alias[0] != '\0')
        << "Font alias is null or empty (path=" << path << ")";

    std::string resolved = ResolveFontPath(path);
    if (resolved.empty()) {
        if (strcmp(source, "user") == 0) {
            LOG(FATAL) << "Font file not found: " << path
                       << " (alias=" << alias << ")";
        }
        // builtin 字体找不到就静默跳过
        LOG(INFO) << "Builtin font not found, skipping: " << path;
        return;
    }

    // 检查 alias 是否已注册
    CHECK(font_slots->find(alias) == font_slots->end())
        << "Duplicate font alias: \"" << alias << "\" from source=" << source
        << " path=" << path;

    FontSlot slot;
    InitOneFontSlot(alias, resolved, ttc_index, &slot);

    auto result = font_slots->emplace(alias, std::move(slot));
    CHECK(result.second) << "Duplicate font alias (internal): " << alias;
    font_order->push_back(alias);
}

void Renderer::InitFonts(
    const std::vector<std::tuple<const char*, int, const char*>>& font_entries,
    const std::vector<std::tuple<const char*, int, const char*>>& default_fonts) {
    // 用户字体最多 10 种
    CHECK_LE(static_cast<int>(font_entries.size()), 10)
        << "Too many fonts: " << font_entries.size()
        << " (max 10)";

    // 用户字体内部别名查重
    for (size_t i = 0; i < font_entries.size(); ++i) {
        for (size_t j = i + 1; j < font_entries.size(); ++j) {
            CHECK(strcmp(std::get<2>(font_entries[i]),
                         std::get<2>(font_entries[j])) != 0)
                << "Duplicate font alias: " << std::get<2>(font_entries[i]);
        }
    }

    // === 第一步：注册用户字体 ===
    for (const auto& fe : font_entries) {
        RegisterFont(std::get<0>(fe),
                      std::get<1>(fe),
                      std::get<2>(fe),
                      "user",
                      &font_slots_, &font_order_);
    }

    // === 第二步：注册内置默认字体（共享别名空间） ===
    for (const auto& de : default_fonts) {
        RegisterFont(std::get<0>(de),
                      std::get<1>(de),
                      std::get<2>(de),
                      "builtin",
                      &font_slots_, &font_order_);
    }

    // 至少一种字体可用
    CHECK(!font_slots_.empty())
        << "No fonts loaded (user nor built-in). "
        << "Provide at least one font via JPOV::Config::fonts.";
}

// ==================== FontSlot 查找 ====================

Renderer::FontSlot* Renderer::FindFontSlot(const std::string& alias) {
    if (!alias.empty()) {
        auto it = font_slots_.find(alias);
        if (it != font_slots_.end()) {
            return &it->second;
        }
        // 别名不存在 → crash（用户指定了不存在的字体别名）
        std::string registered;
        for (const auto& a : font_order_) {
            if (!registered.empty()) registered += ", ";
            registered += a;
        }
        LOG(FATAL) << "Unknown font alias: \"" << alias
                   << "\". Registered aliases: "
                   << (font_order_.empty() ? "(none)" : registered);
    }
    // 空别名 → 返回第一个
    if (font_order_.empty()) return nullptr;
    return &font_slots_.at(font_order_[0]);
}

// ==================== Atlas 上传 ====================

void Renderer::UploadAtlas(FontSlot& slot, int level) {
    if (!slot.manager.has_value() || !slot.manager->loaded()) return;
    if (!slot.manager->atlas_dirty(level) || !slot.atlas_tex[level]) return;
    // 全量更新 GL 纹理（4096x4096 不太大，全量上传即可）
    glBindTexture(GL_TEXTURE_2D, slot.atlas_tex[level]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    FontManager::kAtlasDim, FontManager::kAtlasDim,
                    GL_RED, GL_UNSIGNED_BYTE,
                    slot.manager->atlas_pixels(level).data());
    glBindTexture(GL_TEXTURE_2D, 0);
    slot.manager->mark_atlas_clean(level);
    LOG_EVERY_N(INFO, FontManager::kUploadLogInterval) << "UploadAtlas[" << level << "]: uploaded "
        << FontManager::kAtlasDim << "x" << FontManager::kAtlasDim;
}

void Renderer::UploadAllDirty(FontSlot& slot) {
    if (!slot.manager.has_value()) return;
    for (int lv = 0; lv < FontManager::kNumLevels; ++lv) {
        UploadAtlas(slot, lv);
    }
}

// ==================== DrawText2D ====================

void Renderer::DrawText2D(const Text2DCommand& cmd) {
    // 按别名查找字体
    FontSlot* slot = FindFontSlot(cmd.font_alias);
    if (!slot || !slot->manager.has_value() || !slot->manager->loaded()) {
        LOG_EVERY_N(WARNING, FontManager::kNotLoadedLogInterval)
            << "Text2D: font not loaded for alias=\"" << cmd.font_alias << "\", skipping";
        return;
    }

    CHECK_GT(cmd.font_size, 0.0f);

    // GenerateTextVertices 内部执行多级 atlas 选择 + 包围盒计算 + 顶点生成
    int selected_level = 0;
    std::vector<float> verts;
    bool ok = slot->manager->GenerateTextVertices(
        cmd.text, cmd.font_size,
        cmd.pos.x(), cmd.pos.y(),
        static_cast<int>(cmd.alignment),
        fbo_w_, fbo_h_,
        &selected_level,
        &verts);

    if (!ok || verts.empty()) {
        return;
    }

    // 上传新光栅化的字形到 GL atlas（所有脏层）
    UploadAllDirty(*slot);

    // 上传顶点数据到 VBO
    unsigned int prog = TextProg();
    glUseProgram(prog);
    glUniform2f(glGetUniformLocation(prog, "uFboSize"),
                static_cast<float>(fbo_w_), static_cast<float>(fbo_h_));
    glUniform4f(glGetUniformLocation(prog, "uColor"),
                cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);
    glUniform1i(glGetUniformLocation(prog, "uTexture"), 0);

    // 绑定对应层级的纹理
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, slot->atlas_tex[selected_level]);

    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                 verts.data(), GL_DYNAMIC_DRAW);

    // 位置 (location 0)  | 纹理坐标 (location 1)
    // x,y,u,v interleaved, stride = 4 floats
    constexpr int kStride = 4 * static_cast<int>(sizeof(float));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, kStride, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, kStride,
                          (void*)(2 * sizeof(float)));

    int vert_count = static_cast<int>(verts.size()) / 4;
    glDrawArrays(GL_TRIANGLES, 0, vert_count);

    GLenum draw_err = glGetError();
    if (draw_err != GL_NO_ERROR) {
        LOG_FIRST_N(WARNING, 1) << "GL error after DrawText2D: " << draw_err;
    }

    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Renderer::BeginFrame(int render_w, int render_h) {
    EnsureFBO(render_w, render_h);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, render_w, render_h);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

void Renderer::Render(const RenderCommandList& cmds,
                       const WindowInfo& winfo) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // ---- 补全 viewport 和 3D FBO 尺寸 ----
    // 如果 viewport 全 0，用窗口尺寸补全为全屏
    Camera cam = cmds.camera;
    if (cam.viewport_width == 0.0f && cam.viewport_height == 0.0f) {
        cam.viewport_x = 0.0f;
        cam.viewport_y = 0.0f;
        cam.viewport_width = winfo.width;
        cam.viewport_height = winfo.height;
    }
    // 如果 3D FBO 尺寸未设置，用主 FBO 尺寸
    int fbo_3d_w = static_cast<int>(cam.fbo_3d_width_);
    int fbo_3d_h = static_cast<int>(cam.fbo_3d_height_);
    if (fbo_3d_w <= 0 || fbo_3d_h <= 0) {
        fbo_3d_w = fbo_w_;
        fbo_3d_h = fbo_h_;
    }

    // ---- 检查是否有 3D 指令 ----
    bool has_3d = false;
    for (const auto& [type, idx] : cmds.order) {
        (void)idx;
        if (type == DrawCommandType::kTriangle3D ||
            type == DrawCommandType::kStrip3D ||
            type == DrawCommandType::kLine3D ||
            type == DrawCommandType::kText3D ||
            type == DrawCommandType::kObject3D) {
            has_3d = true;
            break;
        }
    }

    if (has_3d) {
        // ---- 保存并设置 3D 所需 GL 状态 ----
        // 用 glPushAttrib 批保存所有受影响的 GL 状态位。
        // GL_ENABLE_BIT:   GL_DEPTH_TEST, GL_CULL_FACE
        // GL_VIEWPORT_BIT:  glViewport
        // GL_SCISSOR_BIT:   视口相关
        // GL_CURRENT_BIT:   glMatrixMode 等（其实 3D 改的是 shader uniform，
        //                   不再改矩阵栈了，但保留 push/pop 习惯）
        glPushAttrib(GL_ENABLE_BIT | GL_VIEWPORT_BIT);

        // ---- 第一步：在离屏 MSAA FBO 上绘制所有 3D 指令 ----
        Ensure3DFBO(fbo_3d_w, fbo_3d_h);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_3d_);
        glViewport(0, 0, fbo_3d_w, fbo_3d_h);

        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glDepthFunc(GL_LESS);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CCW);

        // Clear 3D FBO（颜色 + 深度）
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // 先用当前 Camera 计算 MVP（Proj * View，不含 Model）。
        // tile culling 投影光源需要它，必须先于 BuildTileLightIndices。
        BuildMVP(cmds.camera, fbo_3d_w, fbo_3d_h, mvp_);

        // ---- Tile Forward 光照准备 ----
        // 若有 Object3D 需光照（!object_use_default_color），
        // 上传光源 uniform + CPU 端做 tile culling（写入 tile index 纹理）。
        if (!cmds.object_use_default_color && !cmds.object3d.empty()) {
            EnsureTileLighting(fbo_3d_w, fbo_3d_h);
            UploadLightData(cmds);
            BuildTileLightIndices(cmds, fbo_3d_w, fbo_3d_h);
        }

        // 用 3D FBO 尺寸计算 MVP
        Draw3DCommands(cmds, fbo_3d_w, fbo_3d_h);

        // ---- 第二步：MSAA resolve 或直接 blit 到主 FBO ----
        glDisable(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST);

#ifdef JPOV_WITHOUT_MSAA
        // 非 MSAA 路径：从 3D FBO 直接 blit 到主 FBO
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_3d_);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_);
        glBlitFramebuffer(
            0, 0, fbo_3d_w, fbo_3d_h,
            static_cast<int>(cam.viewport_x),
            static_cast<int>(cam.viewport_y),
            static_cast<int>(cam.viewport_x + cam.viewport_width),
            static_cast<int>(cam.viewport_y + cam.viewport_height),
            GL_COLOR_BUFFER_BIT, GL_LINEAR);
#else
        // 先 MSAA resolve 到中间非 MSAA FBO（同尺寸 resolve）
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_3d_);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolve_fbo_3d_);
        glBlitFramebuffer(
            0, 0, fbo_3d_w, fbo_3d_h,
            0, 0, resolve_fbo_3d_w_, resolve_fbo_3d_h_,
            GL_COLOR_BUFFER_BIT, GL_LINEAR);

        // 再从中间 FBO 缩放 blit 到主 FBO（按 viewport）
        glBindFramebuffer(GL_READ_FRAMEBUFFER, resolve_fbo_3d_);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_);
        glBlitFramebuffer(
            0, 0, resolve_fbo_3d_w_, resolve_fbo_3d_h_,
            static_cast<int>(cam.viewport_x),
            static_cast<int>(cam.viewport_y),
            static_cast<int>(cam.viewport_x + cam.viewport_width),
            static_cast<int>(cam.viewport_y + cam.viewport_height),
            GL_COLOR_BUFFER_BIT, GL_LINEAR);
#endif

        // ---- 恢复 GL 状态（回到 2D 绘制前的状态）----
        glPopAttrib();
    }

    // ---- 第四步：绑定主 FBO，绘制所有 2D 指令 ----
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, fbo_w_, fbo_h_);

    for (const auto& [type, idx] : cmds.order) {
        switch (type) {
            case DrawCommandType::kRect2D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.rect2d.size()));
                DrawRect2D(cmds.rect2d[idx]);
                break;
            }
            case DrawCommandType::kPolyline2D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.polyline2d.size()));
                DrawPolyline2D(cmds.polyline2d[idx]);
                break;
            }
            case DrawCommandType::kCircle2D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.circle2d.size()));
                DrawCircle2D(cmds.circle2d[idx]);
                break;
            }
            case DrawCommandType::kText2D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.text2d.size()));
                DrawText2D(cmds.text2d[idx]);
                break;
            }
            case DrawCommandType::kStrip2D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.strip2d.size()));
                DrawStrip2D(cmds.strip2d[idx]);
                break;
            }
            case DrawCommandType::kRoundRect2D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.roundrect2d.size()));
                DrawRoundRect2D(cmds.roundrect2d[idx]);
                break;
            }
            case DrawCommandType::kFillRect2D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.fillrect2d.size()));
                DrawFillRect2D(cmds.fillrect2d[idx]);
                break;
            }
            case DrawCommandType::kArc2D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.arc2d.size()));
                DrawArc2D(cmds.arc2d[idx]);
                break;
            }
            case DrawCommandType::kImage2D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.image2d.size()));
                DrawImage2D(cmds.image2d[idx]);
                break;
            }
            default:
                break;
        }
    }
}

void Renderer::Draw3DCommands(const RenderCommandList& cmds, int fbo_w, int fbo_h) {
    // 先用当前 Camera 计算 MVP（View * Proj，不含 Model）
    BuildMVP(cmds.camera, fbo_w, fbo_h, mvp_);

    // 遍历 order，绘制 3D 指令
    for (const auto& [type, idx] : cmds.order) {
        switch (type) {
            case DrawCommandType::kTriangle3D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.triangle3d.size()));
                DrawTriangle3D(cmds.triangle3d[idx]);
                break;
            }
            case DrawCommandType::kStrip3D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.strip3d.size()));
                DrawStrip3D(cmds.strip3d[idx]);
                break;
            }
            case DrawCommandType::kLine3D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.line3d.size()));
                DrawLine3D(cmds.line3d[idx]);
                break;
            }
            case DrawCommandType::kText3D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.text3d.size()));
                DrawText3D(cmds.text3d[idx]);
                break;
            }
            case DrawCommandType::kObject3D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.object3d.size()));
                DrawObject3D(cmds.object3d[idx], cmds);
                break;
            }
            default:
                break;
        }
    }
}

// ==================== 3D 绘制方法 ====================

void Renderer::DrawTriangle3D(const Triangle3DCommand& cmd) {
    // 3 个顶点 × xyz = 9 floats
    float verts[9] = {
        cmd.p1.x(), cmd.p1.y(), cmd.p1.z(),
        cmd.p2.x(), cmd.p2.y(), cmd.p2.z(),
        cmd.p3.x(), cmd.p3.y(), cmd.p3.z(),
    };

    unsigned int prog = Solid3DProg();
    glUseProgram(prog);
    glUniformMatrix4fv(glGetUniformLocation(prog, "uMVP"),
                       1, GL_FALSE, mvp_);
    glUniform4f(glGetUniformLocation(prog, "uColor"),
                cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);

    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void Renderer::DrawStrip3D(const Strip3DCommand& cmd) {
    int n = static_cast<int>(cmd.vertices.size());
    if (n < 3) return;

    // 截断到 3000 顶点上限
    int capped_n = (n > kMaxStripVertices) ? kMaxStripVertices : n;

    // 使用真正的 GL_TRIANGLE_STRIP，直接上传原始顶点序列。
    // GL_TRIANGLE_STRIP 的卷绕顺序为：三角形 i 由顶点 (i, i+1, i+2) 构成，
    // 每个三角形的卷绕方向取决于顶点索引的奇偶性——奇数三角形保持 CCW，
    // 偶数三角形自动反转卷绕以维持面朝向的一致性。
    // 因此无需 save/restore CULL_FACE。

    int total_floats = capped_n * 3;  // capped_n 个顶点 × 3 floats

    unsigned int prog = Solid3DProg();
    glUseProgram(prog);
    glUniformMatrix4fv(glGetUniformLocation(prog, "uMVP"),
                       1, GL_FALSE, mvp_);
    glUniform4f(glGetUniformLocation(prog, "uColor"),
                cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);

    // 上传到专用 VBO
    glBindBuffer(GL_ARRAY_BUFFER, strip_vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(total_floats * sizeof(float)),
                 cmd.vertices.data(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, capped_n);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void Renderer::DrawStrip2D(const Strip2DCommand& cmd) {
    int n = static_cast<int>(cmd.vertices.size());
    if (n < 3) return;

    int capped_n = (n > kMaxStrip2DVertices) ? kMaxStrip2DVertices : n;
    int total_floats = capped_n * 2;  // Vec2f = 2 floats per vertex

    unsigned int prog = SolidProg();
    glUseProgram(prog);
    glUniform2f(glGetUniformLocation(prog, "uFboSize"),
                static_cast<float>(fbo_w_), static_cast<float>(fbo_h_));
    glUniform4f(glGetUniformLocation(prog, "uColor"),
                cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);

    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(total_floats * sizeof(float)),
                 cmd.vertices.data(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, capped_n);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

// ==================== 圆角矩形填充三角化（共享方法） ====================
//
// 将圆角矩形分成 9 个区域（4 个角 + 4 个边 + 1 个中心矩形），
// 每个 90° 圆角用 kRoundCornerSegments 个扇形三角形逼近。
// 返回的顶点数据用于 GL_TRIANGLES（非 fan）。
// radius=0 时退化返回空（调用方用 GL_TRIANGLE_FAN 处理）。
//
// 三角化拓扑（半径 r）：
//   圆角区域：以圆角内切矩形边界为锚点，计算圆弧上的点
//   边/中心区域：直接用矩形对三角形
std::vector<float> Renderer::TriangulateRoundRectFill(
    const Vec2f& pos, const Vec2f& size, float radius) {
    if (radius <= 0.0f) {
        return {};
    }

    float x0 = pos.x();
    float y0 = pos.y();
    float x1 = x0 + size.x();
    float y1 = y0 + size.y();
    float r = radius;

    // 圆角边界辅助点（内切边界坐标）
    // 左上角: (x0+r, y0+r) 右上角: (x1-r, y0+r)
    // 左下角: (x0+r, y1-r) 右下角: (x1-r, y1-r)
    float cx_inner = x0 + r;
    float cy_inner = y0 + r;
    float cx_outer = x1 - r;
    float cy_outer = y1 - r;

    int max_verts = 4 * (kRoundCornerSegments + 1) * 2  // 4 corners
                  + 4 * 6                                 // 4 edge rects (2 tris each)
                  + 6;                                    // center rect (2 tris)
    std::vector<float> verts;
    verts.reserve(max_verts);

    // ===== 中心矩形 (x0+r, y0+r) ~ (x1-r, y1-r) =====
    float center_x0 = cx_inner;
    float center_y0 = cy_inner;
    float center_x1 = cx_outer;
    float center_y1 = cy_outer;
    if (center_x1 > center_x0 && center_y1 > center_y0) {
        verts.push_back(center_x0); verts.push_back(center_y0);
        verts.push_back(center_x1); verts.push_back(center_y0);
        verts.push_back(center_x1); verts.push_back(center_y1);
        verts.push_back(center_x1); verts.push_back(center_y1);
        verts.push_back(center_x0); verts.push_back(center_y1);
        verts.push_back(center_x0); verts.push_back(center_y0);
    }

    // ===== 4 个边矩形（上、下、左、右） =====
    // 上边: (x0+r, y0) ~ (x1-r, y0+r)
    if (x1 - r > x0 + r) {
        verts.push_back(cx_inner);  verts.push_back(y0);
        verts.push_back(cx_outer);  verts.push_back(y0);
        verts.push_back(cx_outer);  verts.push_back(cy_inner);
        verts.push_back(cx_outer);  verts.push_back(cy_inner);
        verts.push_back(cx_inner);  verts.push_back(cy_inner);
        verts.push_back(cx_inner);  verts.push_back(y0);
    }
    // 下边: (x0+r, y1-r) ~ (x1-r, y1)
    if (x1 - r > x0 + r) {
        verts.push_back(cx_inner);  verts.push_back(cy_outer);
        verts.push_back(cx_outer);  verts.push_back(cy_outer);
        verts.push_back(cx_outer);  verts.push_back(y1);
        verts.push_back(cx_outer);  verts.push_back(y1);
        verts.push_back(cx_inner);  verts.push_back(y1);
        verts.push_back(cx_inner);  verts.push_back(cy_outer);
    }
    // 左边: (x0, y0+r) ~ (x0+r, y1-r)
    if (y1 - r > y0 + r) {
        verts.push_back(x0);       verts.push_back(cy_inner);
        verts.push_back(cx_inner); verts.push_back(cy_inner);
        verts.push_back(cx_inner); verts.push_back(cy_outer);
        verts.push_back(cx_inner); verts.push_back(cy_outer);
        verts.push_back(x0);       verts.push_back(cy_outer);
        verts.push_back(x0);       verts.push_back(cy_inner);
    }
    // 右边: (x1-r, y0+r) ~ (x1, y1-r)
    if (y1 - r > y0 + r) {
        verts.push_back(cx_outer); verts.push_back(cy_inner);
        verts.push_back(x1);       verts.push_back(cy_inner);
        verts.push_back(x1);       verts.push_back(cy_outer);
        verts.push_back(x1);       verts.push_back(cy_outer);
        verts.push_back(cx_outer); verts.push_back(cy_outer);
        verts.push_back(cx_outer); verts.push_back(cy_inner);
    }

    // ===== 4 个圆角区域（扇形三角形） =====
    // 左上角：圆心 (x0+r, y0+r)，从 180° 到 270°
    for (int seg = 0; seg < kRoundCornerSegments; ++seg) {
        double a0 = 3.14159265358979323846 * (180.0 + 90.0 * seg / kRoundCornerSegments) / 180.0;
        double a1 = 3.14159265358979323846 * (180.0 + 90.0 * (seg + 1) / kRoundCornerSegments) / 180.0;
        float px0 = cx_inner + r * std::cos(a0);
        float py0 = cy_inner + r * std::sin(a0);
        float px1 = cx_inner + r * std::cos(a1);
        float py1 = cy_inner + r * std::sin(a1);
        verts.push_back(cx_inner); verts.push_back(cy_inner);
        verts.push_back(px0);      verts.push_back(py0);
        verts.push_back(px1);      verts.push_back(py1);
    }
    // 右上角：圆心 (x1-r, y0+r)，从 270° 到 360°
    for (int seg = 0; seg < kRoundCornerSegments; ++seg) {
        double a0 = 3.14159265358979323846 * (270.0 + 90.0 * seg / kRoundCornerSegments) / 180.0;
        double a1 = 3.14159265358979323846 * (270.0 + 90.0 * (seg + 1) / kRoundCornerSegments) / 180.0;
        float px0 = cx_outer + r * std::cos(a0);
        float py0 = cy_inner + r * std::sin(a0);
        float px1 = cx_outer + r * std::cos(a1);
        float py1 = cy_inner + r * std::sin(a1);
        verts.push_back(cx_outer); verts.push_back(cy_inner);
        verts.push_back(px0);      verts.push_back(py0);
        verts.push_back(px1);      verts.push_back(py1);
    }
    // 右下角：圆心 (x1-r, y1-r)，从 0° 到 90°
    for (int seg = 0; seg < kRoundCornerSegments; ++seg) {
        double a0 = 3.14159265358979323846 * (90.0 * seg / kRoundCornerSegments) / 180.0;
        double a1 = 3.14159265358979323846 * (90.0 * (seg + 1) / kRoundCornerSegments) / 180.0;
        float px0 = cx_outer + r * std::cos(a0);
        float py0 = cy_outer + r * std::sin(a0);
        float px1 = cx_outer + r * std::cos(a1);
        float py1 = cy_outer + r * std::sin(a1);
        verts.push_back(cx_outer); verts.push_back(cy_outer);
        verts.push_back(px0);      verts.push_back(py0);
        verts.push_back(px1);      verts.push_back(py1);
    }
    // 左下角：圆心 (x0+r, y1-r)，从 90° 到 180°
    for (int seg = 0; seg < kRoundCornerSegments; ++seg) {
        double a0 = 3.14159265358979323846 * (90.0 + 90.0 * seg / kRoundCornerSegments) / 180.0;
        double a1 = 3.14159265358979323846 * (90.0 + 90.0 * (seg + 1) / kRoundCornerSegments) / 180.0;
        float px0 = cx_inner + r * std::cos(a0);
        float py0 = cy_outer + r * std::sin(a0);
        float px1 = cx_inner + r * std::cos(a1);
        float py1 = cy_outer + r * std::sin(a1);
        verts.push_back(cx_inner); verts.push_back(cy_outer);
        verts.push_back(px0);      verts.push_back(py0);
        verts.push_back(px1);      verts.push_back(py1);
    }

    return verts;
}

void Renderer::DrawRoundRect2D(const RoundRect2DCommand& cmd) {
    // Render a round-topped rectangle using shared CPU-side triangulation
    // via TriangulateRoundRectFill().

    if (cmd.radius <= 0.0f) {
        // Degenerate to plain rectangle via GL_TRIANGLE_FAN
        float x0 = cmd.pos.x();
        float y0 = cmd.pos.y();
        float x1 = x0 + cmd.size.x();
        float y1 = y0 + cmd.size.y();
        float verts[8] = {x0, y0, x1, y0, x1, y1, x0, y1};
        unsigned int prog = SolidProg();
        glUseProgram(prog);
        glUniform2f(glGetUniformLocation(prog, "uFboSize"),
                    static_cast<float>(fbo_w_), static_cast<float>(fbo_h_));
        glUniform4f(glGetUniformLocation(prog, "uColor"),
                    cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);
        glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        glDisableVertexAttribArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        return;
    }

    std::vector<float> verts = TriangulateRoundRectFill(
        cmd.pos, cmd.size, cmd.radius);

    // Render
    int total_verts = static_cast<int>(verts.size()) / 2;
    unsigned int prog = SolidProg();
    glUseProgram(prog);
    glUniform2f(glGetUniformLocation(prog, "uFboSize"),
                static_cast<float>(fbo_w_), static_cast<float>(fbo_h_));
    glUniform4f(glGetUniformLocation(prog, "uColor"),
                cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);

    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                 verts.data(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glDrawArrays(GL_TRIANGLES, 0, total_verts);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void Renderer::DrawFillRect2D(const FillRect2DCommand& cmd) {
    // FillRect2D 的实现策略：
    // 1. 填充部分：直接复用 TriangulateRoundRectFill() 三角化逻辑（用 fill_color）
    // 2. 边框部分：将圆角矩形边框环三角化为 GL_TRIANGLES（用 border_color）
    //
    // 边框环的几何定义：
    //   外圆角矩形 = (pos, size, radius)
    //   内圆角矩形 = (pos + border_width, size - 2*border_width, inner_radius)
    //   其中 inner_radius = max(0, radius - border_width)
    //
    // 边框环三角化：
    //   每个角区域：内外圆弧之间的扇形环
    //   每条边区域：内外矩形边之间的矩形带

    static constexpr int kMaxBorderVerts = 4 * kRoundCornerSegments * 6  // 4 corner rings, 2 tris/seg
                                          + 4 * 6;                      // 4 edge strips

    float x0 = cmd.pos.x();
    float y0 = cmd.pos.y();
    float x1 = x0 + cmd.size.x();
    float y1 = y0 + cmd.size.y();
    float r = cmd.radius;
    float bw = cmd.border_width;

    // ===== 第一步：填充部分 =====
    // 复用 TriangulateRoundRectFill() 产生三角形顶点
    unsigned int prog = SolidProg();
    glUseProgram(prog);
    glUniform2f(glGetUniformLocation(prog, "uFboSize"),
                static_cast<float>(fbo_w_), static_cast<float>(fbo_h_));
    glUniform4f(glGetUniformLocation(prog, "uColor"),
                cmd.fill_color.r, cmd.fill_color.g,
                cmd.fill_color.b, cmd.fill_color.a);

    if (r <= 0.0f) {
        float verts[8] = {x0, y0, x1, y0, x1, y1, x0, y1};
        glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        glDisableVertexAttribArray(0);
    } else {
        std::vector<float> fill_verts = TriangulateRoundRectFill(
            cmd.pos, cmd.size, cmd.radius);
        int total_verts = static_cast<int>(fill_verts.size()) / 2;
        glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(fill_verts.size() * sizeof(float)),
                     fill_verts.data(), GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
        glDrawArrays(GL_TRIANGLES, 0, total_verts);
        glDisableVertexAttribArray(0);
    }

    // ===== 第二步：边框环三角化 =====
    if (bw <= 0.0f) {
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        return;
    }

    float in_bw = std::min(bw, std::min(cmd.size.x(), cmd.size.y()) * 0.5f);
    float inner_r = std::max(0.0f, r - in_bw);

    float ix0 = x0 + in_bw;
    float iy0 = y0 + in_bw;
    float ix1 = x1 - in_bw;
    float iy1 = y1 - in_bw;

    // 外圆角边界辅助点
    float cx_inner = x0 + r;
    float cy_inner = y0 + r;
    float cx_outer = x1 - r;
    float cy_outer = y1 - r;

    // 内圆角边界辅助点
    float icx_inner = ix0 + inner_r;
    float icy_inner = iy0 + inner_r;
    float icx_outer = ix1 - inner_r;
    float icy_outer = iy1 - inner_r;

    std::vector<float> border_verts;
    border_verts.reserve(kMaxBorderVerts);

    // ===== 边框边区域（矩形带） =====
    // 上边：外 (x0+r, y0) ~ (x1-r, y0) vs 内 (ix0+inner_r, iy0) ~ (ix1-inner_r, iy0)
    float outer_top_left = x0 + r;
    float outer_top_right = x1 - r;
    float inner_top_left = ix0 + inner_r;
    float inner_top_right = ix1 - inner_r;
    if (outer_top_right > outer_top_left && inner_top_right > inner_top_left) {
        border_verts.push_back(outer_top_left);  border_verts.push_back(y0);
        border_verts.push_back(outer_top_right); border_verts.push_back(y0);
        border_verts.push_back(inner_top_right); border_verts.push_back(iy0);
        border_verts.push_back(inner_top_right); border_verts.push_back(iy0);
        border_verts.push_back(inner_top_left);  border_verts.push_back(iy0);
        border_verts.push_back(outer_top_left);  border_verts.push_back(y0);
    }
    // 下边
    float outer_bot_left = x0 + r;
    float outer_bot_right = x1 - r;
    float inner_bot_left = ix0 + inner_r;
    float inner_bot_right = ix1 - inner_r;
    if (outer_bot_right > outer_bot_left && inner_bot_right > inner_bot_left) {
        border_verts.push_back(outer_bot_left);  border_verts.push_back(y1);
        border_verts.push_back(inner_bot_left);  border_verts.push_back(iy1);
        border_verts.push_back(inner_bot_right); border_verts.push_back(iy1);
        border_verts.push_back(inner_bot_right); border_verts.push_back(iy1);
        border_verts.push_back(outer_bot_right); border_verts.push_back(y1);
        border_verts.push_back(outer_bot_left);  border_verts.push_back(y1);
    }
    // 左边
    float outer_left_top = y0 + r;
    float outer_left_bot = y1 - r;
    float inner_left_top = iy0 + inner_r;
    float inner_left_bot = iy1 - inner_r;
    if (outer_left_bot > outer_left_top && inner_left_bot > inner_left_top) {
        border_verts.push_back(x0);       border_verts.push_back(outer_left_top);
        border_verts.push_back(ix0);      border_verts.push_back(inner_left_top);
        border_verts.push_back(ix0);      border_verts.push_back(inner_left_bot);
        border_verts.push_back(ix0);      border_verts.push_back(inner_left_bot);
        border_verts.push_back(x0);       border_verts.push_back(outer_left_bot);
        border_verts.push_back(x0);       border_verts.push_back(outer_left_top);
    }
    // 右边
    float outer_right_top = y0 + r;
    float outer_right_bot = y1 - r;
    float inner_right_top = iy0 + inner_r;
    float inner_right_bot = iy1 - inner_r;
    if (outer_right_bot > outer_right_top && inner_right_bot > inner_right_top) {
        border_verts.push_back(x1);       border_verts.push_back(outer_right_top);
        border_verts.push_back(x1);       border_verts.push_back(outer_right_bot);
        border_verts.push_back(ix1);      border_verts.push_back(inner_right_bot);
        border_verts.push_back(ix1);      border_verts.push_back(inner_right_bot);
        border_verts.push_back(ix1);      border_verts.push_back(inner_right_top);
        border_verts.push_back(x1);       border_verts.push_back(outer_right_top);
    }

    // ===== 边框圆角区域（扇形环） =====
    auto add_ring_segment = [&](float ocx, float ocy, float icx, float icy,
                                double start_deg, double end_deg) {
        double a0 = 3.14159265358979323846 * start_deg / 180.0;
        double a1 = 3.14159265358979323846 * end_deg / 180.0;
        float ox0 = ocx + r * std::cos(a0);
        float oy0 = ocy + r * std::sin(a0);
        float ox1 = ocx + r * std::cos(a1);
        float oy1 = ocy + r * std::sin(a1);
        float ix0_ = icx + inner_r * std::cos(a0);
        float iy0_ = icy + inner_r * std::sin(a0);
        float ix1_ = icx + inner_r * std::cos(a1);
        float iy1_ = icy + inner_r * std::sin(a1);
        border_verts.push_back(ox0);  border_verts.push_back(oy0);
        border_verts.push_back(ox1);  border_verts.push_back(oy1);
        border_verts.push_back(ix0_); border_verts.push_back(iy0_);
        border_verts.push_back(ix0_); border_verts.push_back(iy0_);
        border_verts.push_back(ox1);  border_verts.push_back(oy1);
        border_verts.push_back(ix1_); border_verts.push_back(iy1_);
    };

    if (inner_r > 0.0f) {
        // 左上角
        for (int seg = 0; seg < kRoundCornerSegments; ++seg) {
            add_ring_segment(
                cx_inner, cy_inner, icx_inner, icy_inner,
                180.0 + 90.0 * seg / kRoundCornerSegments,
                180.0 + 90.0 * (seg + 1) / kRoundCornerSegments);
        }
        // 右上角
        for (int seg = 0; seg < kRoundCornerSegments; ++seg) {
            add_ring_segment(
                cx_outer, cy_inner, icx_outer, icy_inner,
                270.0 + 90.0 * seg / kRoundCornerSegments,
                270.0 + 90.0 * (seg + 1) / kRoundCornerSegments);
        }
        // 右下角
        for (int seg = 0; seg < kRoundCornerSegments; ++seg) {
            add_ring_segment(
                cx_outer, cy_outer, icx_outer, icy_outer,
                90.0 * seg / kRoundCornerSegments,
                90.0 * (seg + 1) / kRoundCornerSegments);
        }
        // 左下角
        for (int seg = 0; seg < kRoundCornerSegments; ++seg) {
            add_ring_segment(
                cx_inner, cy_outer, icx_inner, icy_outer,
                90.0 + 90.0 * seg / kRoundCornerSegments,
                90.0 + 90.0 * (seg + 1) / kRoundCornerSegments);
        }
    }

    // Render border
    if (!border_verts.empty()) {
        int total_border = static_cast<int>(border_verts.size()) / 2;
        unsigned int bprog = SolidProg();
        glUseProgram(bprog);
        glUniform2f(glGetUniformLocation(bprog, "uFboSize"),
                    static_cast<float>(fbo_w_), static_cast<float>(fbo_h_));
        glUniform4f(glGetUniformLocation(bprog, "uColor"),
                    cmd.border_color.r, cmd.border_color.g,
                    cmd.border_color.b, cmd.border_color.a);
        glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(border_verts.size() * sizeof(float)),
                     border_verts.data(), GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
        glDrawArrays(GL_TRIANGLES, 0, total_border);
        glDisableVertexAttribArray(0);
    }

    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void Renderer::DrawArc2D(const Arc2DCommand& cmd) {
    // CPU 端三角化圆弧/扇形：
    // 扇形近似：圆心 + 圆弧上 N 个扇形三角形（GL_TRIANGLES）。
    // 跨度角度绝对值 >= 360 时绘制完整圆形。
    // 角度为负时绘制顺时针方向。

    static constexpr int kArcSegs = kArcFullCircleSegments;  // 完整圆的三角形数

    // 计算实际跨度（归一化到 360 度内，支持多圈）
    float abs_span = std::fabs(cmd.span_angle);
    if (abs_span < 1e-6f) return;  // 零跨度，不绘制

    // 如果是完整圆或超过 360 度，绘制完整圆
    if (abs_span >= 360.0f - 1e-6f) {
        // 完整圆：圆形 + N 个三角形
        int tri_count = kArcSegs;
        std::vector<float> verts;
        verts.reserve(static_cast<size_t>(tri_count) * 3 * 2);

        float cx = cmd.center.x();
        float cy = cmd.center.y();
        float r = cmd.radius;

        double start_rad = 0.0;
        double step = 2.0 * 3.14159265358979323846 / kArcSegs;
        for (int i = 0; i < kArcSegs; ++i) {
            double a0 = start_rad + i * step;
            double a1 = start_rad + (i + 1) * step;
            float px0 = cx + r * std::cos(a0);
            float py0 = cy + r * std::sin(a0);
            float px1 = cx + r * std::cos(a1);
            float py1 = cy + r * std::sin(a1);
            verts.push_back(cx);  verts.push_back(cy);
            verts.push_back(px0); verts.push_back(py0);
            verts.push_back(px1); verts.push_back(py1);
        }

        int total_verts = static_cast<int>(verts.size()) / 2;
        unsigned int prog = SolidProg();
        glUseProgram(prog);
        glUniform2f(glGetUniformLocation(prog, "uFboSize"),
                    static_cast<float>(fbo_w_), static_cast<float>(fbo_h_));
        glUniform4f(glGetUniformLocation(prog, "uColor"),
                    cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);
        glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                     verts.data(), GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
        glDrawArrays(GL_TRIANGLES, 0, total_verts);
        glDisableVertexAttribArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        return;
    }

    // 非完整圆：扇形近似
    // 三角形数 = ceil(abs_span / 360 * kArcSegs)，至少 3
    float ratio = abs_span / 360.0f;
    int tri_count = std::max(3, static_cast<int>(kArcSegs * ratio + 0.5f));

    float cx = cmd.center.x();
    float cy = cmd.center.y();
    float r = cmd.radius;

    double start_rad = 3.14159265358979323846 * cmd.start_angle / 180.0;
    double span_rad = 3.14159265358979323846 * cmd.span_angle / 180.0;
    double step = span_rad / tri_count;

    std::vector<float> verts;
    verts.reserve(static_cast<size_t>(tri_count) * 3 * 2);

    for (int i = 0; i < tri_count; ++i) {
        double a0 = start_rad + i * step;
        double a1 = start_rad + (i + 1) * step;
        float px0 = cx + r * std::cos(a0);
        float py0 = cy + r * std::sin(a0);
        float px1 = cx + r * std::cos(a1);
        float py1 = cy + r * std::sin(a1);
        verts.push_back(cx);  verts.push_back(cy);
        verts.push_back(px0); verts.push_back(py0);
        verts.push_back(px1); verts.push_back(py1);
    }

    int total_verts = static_cast<int>(verts.size()) / 2;
    unsigned int prog = SolidProg();
    glUseProgram(prog);
    glUniform2f(glGetUniformLocation(prog, "uFboSize"),
                static_cast<float>(fbo_w_), static_cast<float>(fbo_h_));
    glUniform4f(glGetUniformLocation(prog, "uColor"),
                cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);
    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                 verts.data(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glDrawArrays(GL_TRIANGLES, 0, total_verts);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void Renderer::DrawLine3D(const Line3DCommand& cmd) {
    float verts[6] = {
        cmd.p1.x(), cmd.p1.y(), cmd.p1.z(),
        cmd.p2.x(), cmd.p2.y(), cmd.p2.z(),
    };

    unsigned int prog = Solid3DProg();
    glUseProgram(prog);
    glUniformMatrix4fv(glGetUniformLocation(prog, "uMVP"),
                       1, GL_FALSE, mvp_);
    glUniform4f(glGetUniformLocation(prog, "uColor"),
                cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);

    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glDrawArrays(GL_LINES, 0, 2);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void Renderer::DrawText3D(const Text3DCommand& cmd) {
    (void)cmd;
    LOG_FIRST_N(WARNING, 1) << "DrawText3D: not yet implemented, skipping";
}

// 上传全部点光源数据到 lighting shader 的 flat uniform 数组。
//
// 每帧在 BuildTileLightIndices 之前调用一次（非逐 object 热路径，可接受
// 每光源 3 次 snprintf + glGetUniformLocation）。ShaderManager 会缓存
// uniform location，避免重复字符串查找。
//
// 只上传前 kMaxTotalLights_ 个光源；超限提示由 BuildTileLightIndices 负责
//（这是唯一 warning 触发点）。
//
// Pre-condition: MeshLighting3DProg() 已注册（首次调用会触发编译）。
void Renderer::UploadLightData(const RenderCommandList& cmds) {
    unsigned int prog = MeshLighting3DProg();
    glUseProgram(prog);

    const int total = static_cast<int>(cmds.point_lights.size());
    const int clamped = std::min(total, kMaxTotalLights_);

    char buf[64];
    for (int i = 0; i < clamped; ++i) {
        const PointLight& l = cmds.point_lights[i];
        snprintf(buf, sizeof(buf), "uLights[%d].position", i);
        glUniform3f(shader_mgr_.GetUniform(prog, buf),
                    l.position.x(), l.position.y(), l.position.z());
        snprintf(buf, sizeof(buf), "uLights[%d].color", i);
        glUniform3f(shader_mgr_.GetUniform(prog, buf),
                    l.color.r, l.color.g, l.color.b);
        snprintf(buf, sizeof(buf), "uLights[%d].radius", i);
        glUniform1f(shader_mgr_.GetUniform(prog, buf), l.linear_radius);
    }

    // 告诉 shader 当前有效光源数（fragment 用 tile index 与其做边界钳制）
    glUniform1i(shader_mgr_.GetUniform(prog, "uTotalLights"), clamped);
}

// CPU 端 tile culling：
//   遍历 cmds.point_lights（用户已按优先级排好序），把每个光源投影到
//   NDC → 屏幕空间覆盖矩形 → 转换成 tile 坐标范围 → 向每个覆盖 tile 写入
//   light index（先到先得，每 tile 最多 kMaxLightsPerTile_ 个，后到的丢弃）。
//
// 输出到 tile_index_tex_（GL_RGBA8），fragment shader 按 gl_FragCoord 查表。
//
// 覆盖是保守近似：投影光源球的 6 个轴向边界点取屏幕 x/y 的 min/max，保证
// 装下球体；光源 radius（线性衰减边界）跨越相机时投影会失真，此时让该光源
// 覆盖整个屏幕，确保不漏光（宁多勿少）。这是近似而非精确锥体裁剪。
void Renderer::BuildTileLightIndices(const RenderCommandList& cmds,
                                     int fbo_w_3d, int fbo_h_3d) {
    if (tile_index_tex_ == 0) return;
    if (tile_grid_w_ <= 0 || tile_grid_h_ <= 0) return;

    const int num_lights = static_cast<int>(cmds.point_lights.size());
    if (num_lights == 0) return;

    // 光源数超上限：只处理前 kMaxTotalLights_ 个，其余忽略（先到先得）。
    // 只 LOG 一次，避免每帧刷屏。
    if (num_lights > kMaxTotalLights_) {
        LOG_FIRST_N(WARNING, 1)
            << "point_lights 数量 " << num_lights << " 超过上限 "
            << kMaxTotalLights_
            << "，仅前 " << kMaxTotalLights_ << " 个光源生效";
    }

    // 1. 准备 tile 网格的 CPU 侧缓冲
    const int tex_w = tile_grid_w_ * kTexelsPerTile_;
    const int tex_h = tile_grid_h_;

    // 每个 tile 当前已放入的光源计数（index = row * tile_grid_w_ + col）
    const int total_tiles = tile_grid_w_ * tile_grid_h_;
    std::vector<uint32_t> tile_counts(static_cast<size_t>(total_tiles), 0);
    // 每个 tile 的光源 index 列表（先到先得，uint8：0-254 有效，255=无光源哨兵）
    struct Cell {
        uint8_t idx[kMaxLightsPerTile_];
    };
    std::vector<Cell> tiles(static_cast<size_t>(total_tiles));
    for (auto& c : tiles) {
        for (auto& v : c.idx) v = kLightIndexSentinel_;
    }

    // 2. 遍历光源（用户已按优先级排好序，先到先得）
    const int clamped = std::min(num_lights, kMaxTotalLights_);
    for (int li = 0; li < clamped; ++li) {
        const PointLight& l = cmds.point_lights[li];
        const Vec3f cx = {l.position.x(), l.position.y(), l.position.z()};
        const float radius = l.linear_radius;
        if (radius <= 0.0f) continue;

        // 投影球心到 clip 坐标
        float c[4];
        c[0] = mvp_[0]*cx.x() + mvp_[4]*cx.y() + mvp_[8]*cx.z()  + mvp_[12];
        c[1] = mvp_[1]*cx.x() + mvp_[5]*cx.y() + mvp_[9]*cx.z()  + mvp_[13];
        c[2] = mvp_[2]*cx.x() + mvp_[6]*cx.y() + mvp_[10]*cx.z() + mvp_[14];
        c[3] = mvp_[3]*cx.x() + mvp_[7]*cx.y() + mvp_[11]*cx.z() + mvp_[15];

        if (c[3] <= 0.0f) continue;  // 球心在相机后方，跳过（保守：可能漏近光源，但避免全屏误覆盖）

        const float inv_w = 1.0f / c[3];
        const float ndc_x = c[0] * inv_w;
        const float ndc_y = c[1] * inv_w;

        // 球心投影到屏幕像素（OpenGL gl_FragCoord 约定：原点左下角，y 向上）：
        //   px = (ndc_x*0.5+0.5) * fbo_w，py = (ndc_y*0.5+0.5) * fbo_h
        // 注意：fragment shader 里 gl_FragCoord 也是同一约定（左下原点，y 向上），
        // 因此 tile_row = int(gl_FragCoord.y)/16 与这里的 py 一致。
        const float px = (ndc_x * 0.5f + 0.5f) * static_cast<float>(fbo_w_3d);
        const float py = (ndc_y * 0.5f + 0.5f) * static_cast<float>(fbo_h_3d);

        // 屏幕空间覆盖范围：把光源球（中心 cx，半径 radius）的 6 个轴向
        // 边界点（±X/±Y/±Z）都投影到屏幕像素，取 x/y 的 min/max。
        // 这样覆盖真正保守（球被轴对齐包围盒完全包住），不会漏掉
        // 屏幕空间覆盖范围：把光源球（中心 cx，半径 radius）的 6 个轴向
        // 边界点（±X/±Y/±Z）都投影到屏幕像素，取 x/y 的 min/max。
        // 这样覆盖真正保守（球被轴对齐包围盒完全包住），不会漏掉
        // 实际受光源影响的像素（宁可多覆盖，不可少覆盖）。
        //
        // 若光源半径过大导致球体跨越相机（任一轴向边界点落在相机后方），
        // 投影会失真，此时保守地让该光源覆盖整个屏幕，确保不漏光。
        float pmin_x = px, pmax_x = px;
        float pmin_y = py, pmax_y = py;
        bool crosses_camera = false;
        // 6 轴向边界点
        const Vec3f dirs[6] = {
            { 1,0,0}, {-1,0,0}, {0, 1,0}, {0,-1,0}, {0,0, 1}, {0,0,-1},
        };
        for (int d = 0; d < 6; ++d) {
            const Vec3f bp = {
                cx.x() + dirs[d].x() * radius,
                cx.y() + dirs[d].y() * radius,
                cx.z() + dirs[d].z() * radius,
            };
            float bc[4];
            bc[0] = mvp_[0]*bp.x() + mvp_[4]*bp.y() + mvp_[8]*bp.z()  + mvp_[12];
            bc[1] = mvp_[1]*bp.x() + mvp_[5]*bp.y() + mvp_[9]*bp.z()  + mvp_[13];
            bc[2] = mvp_[2]*bp.x() + mvp_[6]*bp.y() + mvp_[10]*bp.z() + mvp_[14];
            bc[3] = mvp_[3]*bp.x() + mvp_[7]*bp.y() + mvp_[11]*bp.z() + mvp_[15];
            if (bc[3] <= 0.0f) {
                crosses_camera = true;
                continue;
            }
            const float biw = 1.0f / bc[3];
            const float bpx = (bc[0]*biw*0.5f + 0.5f) * static_cast<float>(fbo_w_3d);
            const float bpy = (bc[1]*biw*0.5f + 0.5f) * static_cast<float>(fbo_h_3d);
            pmin_x = std::min(pmin_x, bpx);
            pmax_x = std::max(pmax_x, bpx);
            pmin_y = std::min(pmin_y, bpy);
            pmax_y = std::max(pmax_y, bpy);
        }

        int min_px_x, max_px_x, min_px_y, max_px_y;
        if (crosses_camera) {
            // 球跨越相机：保守覆盖整个屏幕
            min_px_x = 0; max_px_x = fbo_w_3d;
            min_px_y = 0; max_px_y = fbo_h_3d;
        } else {
            min_px_x = static_cast<int>(std::floor(pmin_x)) - 1;
            max_px_x = static_cast<int>(std::ceil(pmax_x)) + 1;
            min_px_y = static_cast<int>(std::floor(pmin_y)) - 1;
            max_px_y = static_cast<int>(std::ceil(pmax_y)) + 1;
        }

        // 3. 转换成 tile 范围
        const int min_tc = std::max(0, min_px_x / kTileSize16_);
        const int max_tc = std::min(tile_grid_w_ - 1, max_px_x / kTileSize16_);
        const int min_tr = std::max(0, min_px_y / kTileSize16_);
        const int max_tr = std::min(tile_grid_h_ - 1, max_px_y / kTileSize16_);
        if (min_tc > max_tc || min_tr > max_tr) continue;

        // 4. 向覆盖的每个 tile 写入 light index（先到先得）
        for (int tr = min_tr; tr <= max_tr; ++tr) {
            for (int tc = min_tc; tc <= max_tc; ++tc) {
                const int t = tr * tile_grid_w_ + tc;
                uint32_t& count = tile_counts[t];
                if (count >= kMaxLightsPerTile_) continue;  // 满了，丢弃（后到视为低优先级）
                tiles[t].idx[count] = static_cast<uint8_t>(li);
                ++count;
            }
        }
    }

    // 5. 打包写入 tile 纹理（每 tile 4 个 texel，每 texel RGBA 各 1 个 uint8 index）
    std::vector<uint8_t> packed(static_cast<size_t>(tex_w) * tex_h * 4,
                                kLightIndexSentinel_);
    for (int tr = 0; tr < tile_grid_h_; ++tr) {
        for (int tc = 0; tc < tile_grid_w_; ++tc) {
            const int t = tr * tile_grid_w_ + tc;
            const Cell& cell = tiles[t];
            for (int k = 0; k < kTexelsPerTile_; ++k) {
                const int gx = tc * kTexelsPerTile_ + k;
                uint8_t* px = &packed[(static_cast<size_t>(tr) * tex_w + gx) * 4];
                px[0] = cell.idx[k * 4 + 0];
                px[1] = cell.idx[k * 4 + 1];
                px[2] = cell.idx[k * 4 + 2];
                px[3] = cell.idx[k * 4 + 3];
            }
        }
    }

    glBindTexture(GL_TEXTURE_2D, tile_index_tex_);
    // 全量上传（packed 已含全部哨兵填充，避免先清后写的双重上传）。
    // 每帧一次：tile 纹理很小（1280×720 → 320×45 ≈ 57KB），
    // 全量 glTexImage2D 开销可忽略，且实现简单直接（不做增量 glTexSubImage2D）。
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex_w, tex_h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, packed.data());
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Renderer::DrawObject3D(const Object3DCommand& cmd,
                              const RenderCommandList& cmds) {
    // 1. 从 mesh 管理器取 GPU mesh 句柄（VAO/VBO/EBO 已在注册时绑定好属性）
    const GPUMesh* mesh = mesh_mgr_.GetMesh(cmd.mesh_id);
    CHECK(mesh != nullptr) << "DrawObject3D: mesh_id " << cmd.mesh_id
                           << " 未注册（DrawObject3D 前需先 RegisterMesh）";
    CHECK_GT(mesh->vao, 0u);

    // 2. 校验 up/front 非零且不平行（模型基向量需要）
    const float up_len =
        std::sqrt(cmd.up.x()*cmd.up.x() + cmd.up.y()*cmd.up.y() + cmd.up.z()*cmd.up.z());
    const float fr_len =
        std::sqrt(cmd.front.x()*cmd.front.x() + cmd.front.y()*cmd.front.y() + cmd.front.z()*cmd.front.z());
    CHECK_GT(up_len, 1e-8f) << "DrawObject3D: up 向量不能为零";
    CHECK_GT(fr_len, 1e-8f) << "DrawObject3D: front 向量不能为零";

    // 3. 构建 Model 矩阵，并合成 MVP = mvp_(Proj*View) * Model
    float model[16], mvp[16];
    BuildModelMatrix(cmd.center, cmd.up, cmd.front, model);
    Mat4Mul(mvp_, model, mvp);

    // 4. 纹理 or 纯色着色
    const bool textured = (cmd.texture_id != 0);

    // 决定着色路径：
    //   纹理模式 → 纹理 shader（不受 object_use_default_color 影响）
    //   纯色 + object_use_default_color → 旧纯色 shader（兼容现有 gold test）
    //   纯色 + !object_use_default_color → 光照 shader（Blinn-Phong）
    const bool use_lighting = !textured && !cmds.object_use_default_color;

    if (use_lighting) {
        // 光照模式：需要 normal 属性
        CHECK(MeshHasFlag(mesh->flags, MeshVertexFlags::kNormal))
            << "DrawObject3D: 光照模式需要 kNormal 属性，"
            << "但 mesh_id=" << cmd.mesh_id << " 不含 normal；"
            << "可设置 object_use_default_color=true 使用纯色渲染";

        unsigned int prog = MeshLighting3DProg();
        glUseProgram(prog);

        // MVP（含 Model）
        glUniformMatrix4fv(glGetUniformLocation(prog, "uMVP"),
                           1, GL_FALSE, mvp);
        // Model 矩阵（单独传入，供 world-space position 计算）
        glUniformMatrix4fv(glGetUniformLocation(prog, "uModel"),
                           1, GL_FALSE, model);
        // 模型颜色
        glUniform3f(glGetUniformLocation(prog, "uModelColor"),
                    cmd.default_color.r, cmd.default_color.g, cmd.default_color.b);
        // 相机位置（specular 需要）
        glUniform3f(glGetUniformLocation(prog, "uCameraPos"),
                    cmds.camera.position.x(), cmds.camera.position.y(),
                    cmds.camera.position.z());
        // 高光光滑度
        glUniform1f(glGetUniformLocation(prog, "uShininess"), 64.0f);

        // 光源数据与 uTotalLights 已在 UploadLightData（每帧一次）上传。
        // 这里只需绑定 tile index 纹理（fragment shader 按像素查表获取
        // 本 tile 命中的光源 index 列表）。
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tile_index_tex_);
        glUniform1i(glGetUniformLocation(prog, "uTileLightIndices"), 0);
    } else if (textured) {
        // 纹理模式：mesh 必须含 UV 属性
        CHECK(MeshHasFlag(mesh->flags, MeshVertexFlags::kUV))
            << "DrawObject3D: texture_id 非 0 但 mesh 无 kUV 属性，无法纹理采样";
        unsigned int gl_tex = texture_mgr_.GetGLTexture(cmd.texture_id);
        CHECK_NE(gl_tex, 0u)
            << "DrawObject3D: texture_id " << cmd.texture_id << " 未注册";

        unsigned int prog = TexturedMesh3DProg();
        glUseProgram(prog);
        glUniformMatrix4fv(glGetUniformLocation(prog, "uMVP"),
                           1, GL_FALSE, mvp);
        glUniform4f(glGetUniformLocation(prog, "uTint"),
                    cmd.default_color.r, cmd.default_color.g,
                    cmd.default_color.b, cmd.default_color.a);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, gl_tex);
        glUniform1i(glGetUniformLocation(prog, "uTexture"), 0);
    } else {
        // 旧纯色模式（object_use_default_color=true 时走此路径）
        unsigned int prog = Solid3DProg();
        glUseProgram(prog);
        glUniformMatrix4fv(glGetUniformLocation(prog, "uMVP"),
                           1, GL_FALSE, mvp);
        glUniform4f(glGetUniformLocation(prog, "uColor"),
                    cmd.default_color.r, cmd.default_color.g,
                    cmd.default_color.b, cmd.default_color.a);
    }

    // 5. 发起绘制（indexed 用 EBO，否则按顶点序）
    // 绑定 mesh VAO：属性指针已固化在 VAO 中，无需再设 attrib。
    glBindVertexArray(mesh->vao);
    if (mesh->index_count > 0) {
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(mesh->index_count),
                       GL_UNSIGNED_INT, nullptr);
    } else {
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(mesh->vertex_count));
    }
    glBindVertexArray(0);

    GLenum draw_err = glGetError();
    if (draw_err != GL_NO_ERROR) {
        LOG_FIRST_N(WARNING, 1) << "GL error after DrawObject3D: " << draw_err;
    }
}

void Renderer::Present(GLFWwindow* window, int window_width, int window_height) {
    int fb_w, fb_h;
    glfwGetFramebufferSize(window, &fb_w, &fb_h);
    int src_w = std::min(window_width, fbo_w_);
    int src_h = std::min(window_height, fbo_h_);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, src_w, src_h,
                      0, 0, fb_w, fb_h,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Renderer::EnsureOutputFBO(int win_w, int win_h) {
    if (out_w_ == win_w && out_h_ == win_h && out_fbo_) return;

    CHECK_GT(win_w, 0);
    CHECK_GT(win_h, 0);

    DestroyOutputFBO();

    glGenTextures(1, &out_color_tex_);
    glBindTexture(GL_TEXTURE_2D, out_color_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, win_w, win_h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glGenFramebuffers(1, &out_fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, out_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, out_color_tex_, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    CHECK_EQ(status, GL_FRAMEBUFFER_COMPLETE)
        << "Output FBO failed, status=" << status;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    out_w_ = win_w;
    out_h_ = win_h;
}

void Renderer::SaveScreenshot(int win_w, int win_h, const char* path) {
    geom::EnsurePathForFilename(path);

    std::vector<uint8_t> pixels;
    SaveScreenshotToBuffer(win_w, win_h, &pixels);

    // stb_image_write takes RGBA pixels with stride = 4*width
    // stbi_flip_vertically_on_write handles OpenGL's bottom-left origin
    stbi_flip_vertically_on_write(1);
    int ok = stbi_write_png(path, win_w, win_h, 4, pixels.data(), win_w * 4);
    CHECK_NE(ok, 0) << "Failed to write PNG: " << path;
    LOG(INFO) << "Screenshot saved: " << path
              << " (" << win_w << "x" << win_h << ")";
}

void Renderer::SaveScreenshotToBuffer(int win_w, int win_h,
                                        std::vector<uint8_t>* out_pixels) {
    CHECK_GT(win_w, 0);
    CHECK_GT(win_h, 0);
    CHECK_NE(fbo_, 0u);
    CHECK_NOTNULL(out_pixels);

    // 1. 拉伸到 output FBO 模拟 Present 到窗口的效果
    EnsureOutputFBO(win_w, win_h);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, out_fbo_);
    glViewport(0, 0, win_w, win_h);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    int src_w = std::min(win_w, fbo_w_);
    int src_h = std::min(win_h, fbo_h_);
    glBlitFramebuffer(0, 0, src_w, src_h,
                      0, 0, win_w, win_h,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // 2. 从 output FBO 读回像素
    out_pixels->resize(static_cast<size_t>(win_w) * win_h * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, out_fbo_);
    glReadPixels(0, 0, win_w, win_h, GL_RGBA, GL_UNSIGNED_BYTE, out_pixels->data());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

    // 3. RGBA → BGRA（stb_image_write 期望 RGBA，不需要转换）
    // stb_image_write 接受 RGBA 数据，和 OpenGL 读出的 RGBA 一致。
    // Y 翻转由 stbi_flip_vertically_on_write(1) 在 SaveScreenshot 中处理。
    // 这里不做翻转，让调用者决定。
}

void Renderer::DrawPolyline2D(const Polyline2DCommand& cmd) {
    // Pre-condition:
    //   - vertices 至少 2 个点
    //   - edge_count (vertices.size()-1) ≤ kMaxPolylineEdges
    //   - line_width > 0（像素单位）
    int n = static_cast<int>(cmd.vertices.size());
    CHECK_GE(n, 2);
    int edge_count = n - 1;
    CHECK_LE(edge_count, kMaxPolylineEdges);
    CHECK_GT(cmd.line_width, 0.0f);

    // 每个 quad 6 顶点 + 每个 bridge 6 顶点（2 三角形）
    // 顶点格式：x, y, x, y, ...
    int total_verts = edge_count * 6 + (edge_count - 1) * 6;
    std::vector<float> verts;
    verts.reserve(static_cast<size_t>(total_verts) * 2);

    float half_w = cmd.line_width * 0.5f;

    for (int i = 0; i < edge_count; ++i) {
        const Vec2f& p0 = cmd.vertices[i];
        const Vec2f& p1 = cmd.vertices[i + 1];

        // 边向量
        Vec2f dir = p1 - p0;
        float len = std::sqrt(dir.x() * dir.x() + dir.y() * dir.y());

        // 垂直方向（归一化），len 过短时水平偏移
        Vec2f perp;
        static constexpr float kEpsilon = 1e-6f;
        if (len < kEpsilon) {
            perp = {1.0f, 0.0f};
        } else {
            perp = {-dir.y() / len, dir.x() / len};
        }

        // 四个角点
        Vec2f n0 = p0 + perp * half_w;
        Vec2f n1 = p0 - perp * half_w;
        Vec2f n2 = p1 + perp * half_w;
        Vec2f n3 = p1 - perp * half_w;

        // 两个三角形：n0-n1-n2, n2-n1-n3 (CW)
        // T1
        verts.push_back(n0.x()); verts.push_back(n0.y());
        verts.push_back(n1.x()); verts.push_back(n1.y());
        verts.push_back(n2.x()); verts.push_back(n2.y());
        // T2
        verts.push_back(n2.x()); verts.push_back(n2.y());
        verts.push_back(n1.x()); verts.push_back(n1.y());
        verts.push_back(n3.x()); verts.push_back(n3.y());
        // 每个连接处在 V 形间隙外侧补一个三角形
        if (i + 1 < edge_count) {
            const Vec2f& p2 = cmd.vertices[i + 2];
            Vec2f dn = p2 - p1;
            float ln = std::sqrt(dn.x()*dn.x()+dn.y()*dn.y());
            Vec2f perp_n;
            if (ln < kEpsilon) { perp_n = {1.0f, 0.0f}; }
            else { perp_n = {-dn.y()/ln, dn.x()/ln}; }

            // 用顶点和两段矩形外侧角点构成填充三角形
            // 内侧三角形 (p1, n3, n3_next) 和 (p1, n2_next, n2) 填充间隙
            verts.push_back(p1.x()); verts.push_back(p1.y());
            verts.push_back(n3.x()); verts.push_back(n3.y());
            verts.push_back((p1 - perp_n * half_w).x());
            verts.push_back((p1 - perp_n * half_w).y());

            verts.push_back(p1.x()); verts.push_back(p1.y());
            verts.push_back((p1 + perp_n * half_w).x());
            verts.push_back((p1 + perp_n * half_w).y());
            verts.push_back(n2.x()); verts.push_back(n2.y());
        }
    }

    CHECK_LE(total_verts, kMaxStreamVertices);

    unsigned int prog = SolidProg();
    glUseProgram(prog);
    glUniform2f(glGetUniformLocation(prog, "uFboSize"),
                static_cast<float>(fbo_w_), static_cast<float>(fbo_h_));
    glUniform4f(glGetUniformLocation(prog, "uColor"),
                cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);

    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                 verts.data(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glDrawArrays(GL_TRIANGLES, 0, total_verts);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void Renderer::DrawRect2D(const Rect2DCommand& cmd) {
    float verts[8];
    float x0 = cmd.pos.x();
    float y0 = cmd.pos.y();
    float x1 = x0 + cmd.size.x();
    float y1 = y0 + cmd.size.y();
    verts[0] = x0;
    verts[1] = y0;
    verts[2] = x1;
    verts[3] = y0;
    verts[4] = x1;
    verts[5] = y1;
    verts[6] = x0;
    verts[7] = y1;

    unsigned int prog = SolidProg();
    glUseProgram(prog);
    // uFboSize = NDC 变换参照。必须用 FBO 尺寸（渲染分辨率），
    // 使 NDC 坐标空间与 glViewport 一致，避免 rect 偏移。
    glUniform2f(glGetUniformLocation(prog, "uFboSize"),
                static_cast<float>(fbo_w_), static_cast<float>(fbo_h_));
    glUniform4f(glGetUniformLocation(prog, "uColor"),
                cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);

    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void Renderer::DrawCircle2D(const Circle2DCommand& cmd) {
    static constexpr int kSegments = kCircleFanSegments;
    float verts[(kSegments + 2) * 2];  // fan center + kSegments perimeter points
    float cx = cmd.center.x();
    float cy = cmd.center.y();
    float r = cmd.radius;

    // Center of fan
    verts[0] = cx;
    verts[1] = cy;

    constexpr double kPi = 3.14159265358979323846;
    for (int i = 0; i <= kSegments; ++i) {
        double angle = 2.0 * kPi * static_cast<double>(i) /
                       static_cast<double>(kSegments);
        verts[(i + 1) * 2 + 0] = cx + r * static_cast<float>(cos(angle));
        verts[(i + 1) * 2 + 1] = cy + r * static_cast<float>(sin(angle));
    }

    unsigned int prog = SolidProg();
    glUseProgram(prog);
    glUniform2f(glGetUniformLocation(prog, "uFboSize"),
                static_cast<float>(fbo_w_), static_cast<float>(fbo_h_));
    glUniform4f(glGetUniformLocation(prog, "uColor"),
                cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);

    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glDrawArrays(GL_TRIANGLE_FAN, 0, kSegments + 2);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void Renderer::DrawImage2D(const Image2DCommand& cmd) {
    // 查找纹理
    int tex_w = 0;
    int tex_h = 0;
    bool found = texture_mgr_.GetSize(cmd.texture_id, &tex_w, &tex_h);
    CHECK(found) << "DrawImage2D: texture_id=" << cmd.texture_id
                 << " not registered";
    CHECK_GT(tex_w, 0);
    CHECK_GT(tex_h, 0);

    uint32_t gl_tex = texture_mgr_.GetGLTexture(cmd.texture_id);
    CHECK_NE(gl_tex, 0u) << "DrawImage2D: texture_id=" << cmd.texture_id
                          << " has no GL texture";

    // 构建矩形面片：pos → pos+size，UV (0,0)→(1,1)
    // 顶点格式：[x, y, u, v] interleaved，6 顶点（2 三角形）
    float x0 = cmd.pos.x();
    float y0 = cmd.pos.y();
    float x1 = x0 + cmd.size.x();
    float y1 = y0 + cmd.size.y();

    // T1: (x0,y0)-(x1,y0)-(x1,y1), T2: (x0,y0)-(x1,y1)-(x0,y1)
    // 屏幕坐标 y0=上 y1=下。shader 设 ndc.y = -ndc.y 使 y0→NDC 上方。
    // stb_image 像素第一行=图片顶部, glTexImage2D 把它放在 v=0 (GL 纹理底部),
    // 因此图片顶部对应 v=0, 图片底部对应 v=1。
    // 要把图片顶部映射到屏幕上方 (y0), 底部映射到屏幕下方 (y1):
    //   y0 → v=0, y1 → v=1
    float verts[24] = {
        x0, y0, 0.0f, 0.0f,  // T1: top-left
        x1, y0, 1.0f, 0.0f,  // T1: top-right
        x1, y1, 1.0f, 1.0f,  // T1: bottom-right
        x0, y0, 0.0f, 0.0f,  // T2: top-left
        x1, y1, 1.0f, 1.0f,  // T2: bottom-right
        x0, y1, 0.0f, 1.0f,  // T2: bottom-left
    };

    unsigned int prog = ImageProg();
    glUseProgram(prog);
    glUniform2f(glGetUniformLocation(prog, "uFboSize"),
                static_cast<float>(fbo_w_), static_cast<float>(fbo_h_));
    glUniform4f(glGetUniformLocation(prog, "uTint"),
                cmd.tint.r, cmd.tint.g, cmd.tint.b, cmd.tint.a);
    glUniform1i(glGetUniformLocation(prog, "uTexture"), 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gl_tex);

    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);

    constexpr int kStride = 4 * static_cast<int>(sizeof(float));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, kStride, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, kStride,
                          (void*)(2 * sizeof(float)));

    glDrawArrays(GL_TRIANGLES, 0, 6);

    GLenum draw_err = glGetError();
    if (draw_err != GL_NO_ERROR) {
        LOG_FIRST_N(WARNING, 1) << "GL error after DrawImage2D: " << draw_err;
    }

    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

}  // namespace jpov
