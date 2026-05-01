// JPOV Renderer 实现
//
// OpenGL 资源全生命周期管理与绘制调度。
// 所有 GL 细节沉在此文件内部。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include <glog/logging.h>

namespace {

// ============================================================
// 内嵌 GLSL 源码
// ============================================================

// 顶点着色器：像素坐标 → NDC
const char* kVs = R"glsl(
#version 330 core
layout(location = 0) in vec2 aPos;
uniform vec2 uWindowSize;

void main() {
    vec2 ndc = (aPos / uWindowSize) * 2.0 - 1.0;
    ndc.y = -ndc.y;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
)glsl";

// 片段着色器：纯色
const char* kFs = R"glsl(
#version 330 core
out vec4 FragColor;
uniform vec4 uColor;

void main() {
    FragColor = uColor;
}
)glsl";

// ============================================================
// Shader 工具函数
// ============================================================

unsigned int CompileShader(GLenum type, const char* source) {
    unsigned int shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        const char* type_name = "?";
        if (type == GL_VERTEX_SHADER)      type_name = "VS";
        else if (type == GL_FRAGMENT_SHADER) type_name = "FS";
        LOG(FATAL) << "Shader compile error [" << type_name << "]: " << log;
    }
    return shader;
}

unsigned int LinkProgram(unsigned int vs, unsigned int fs) {
    unsigned int prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);

    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        LOG(FATAL) << "Program link error: " << log;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

unsigned int BuildPolyline2DProgram() {
    unsigned int vs = CompileShader(GL_VERTEX_SHADER, kVs);
    unsigned int fs = CompileShader(GL_FRAGMENT_SHADER, kFs);
    return LinkProgram(vs, fs);
}

// ============================================================
// CPU 端折线宽度展开（miter join）
//
// 输入：折线顶点数组 + 线宽
// 输出：GL_TRIANGLE_STRIP 的顶点序列（2D 像素坐标）
//
// Miter join 原理：
//   相邻两段的法向量 avg → miter 方向
//   miter 长度 = half_width / cos(夹角/2)
//   夹角接近 180° 时 miter 趋于无穷，限制最大长度避免尖刺
// ============================================================

struct Vec2 {
    float x, y;

    Vec2 operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(const Vec2& o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(float s) const { return {x * s, y * s}; }

    float Dot(const Vec2& o) const { return x * o.x + y * o.y; }
    float Cross(const Vec2& o) const { return x * o.y - y * o.x; }
    float Len() const { return std::sqrt(x * x + y * y); }
    Vec2 Unit() const {
        float len = Len();
        if (len < 1e-8f) return {1.0f, 0.0f};
        return {x / len, y / len};
    }
    Vec2 Perp() const { return {-y, x}; }
};

static std::vector<float> BuildThickPolyline(
    const float* pts, size_t n, float line_width)
{
    std::vector<float> out;
    if (n < 2) return out;

    // 最多 (n*2 + (n-1)) 个顶点：每段两个三角形的STRIP需要约 4*段 个顶点
    // 为简化，我们用足够大的预留空间
    out.reserve(n * 4 * 2);

    float half_w = line_width * 0.5f;

    // 计算每个顶点的法向量
    // 对于内部顶点（两端之间的点），法向量 = 前后段法向量的平均（miter）
    // 对于端点，法向量 = 唯一段的垂直方向
    struct VertexNormals {
        Vec2 left;   // 向左侧偏移方向
        Vec2 right;  // 向右侧偏移方向
    };
    std::vector<VertexNormals> normals(n);

    for (size_t i = 0; i < n; ++i) {
        Vec2 dir;
        if (n == 1) {
            dir = {1.0f, 0.0f};
        } else if (i == 0) {
            dir = {pts[2] - pts[0], pts[3] - pts[1]};
        } else if (i == n - 1) {
            dir = {pts[(i-1)*2] - pts[i*2], pts[(i-1)*2+1] - pts[i*2+1]};
        } else {
            Vec2 prev = {pts[(i-1)*2] - pts[i*2], pts[(i-1)*2+1] - pts[i*2+1]};
            Vec2 next = {pts[i*2] - pts[(i+1)*2], pts[i*2+1] - pts[(i+1)*2+1]};
            dir = prev + next;
        }
        dir = dir.Unit();
        Vec2 perp = dir.Perp();

        // 检查是否使用了 miter（相邻段）
        if (i == 0 || i == n - 1 || n == 2) {
            // 端点：直接用垂直方向
            normals[i].left  = perp;
            normals[i].right = {-perp.x, -perp.y};
        } else {
            // 内部点：前后段法向量平均，限制最大 miter 长度
            Vec2 prev_dir = {pts[(i-1)*2] - pts[i*2], pts[(i-1)*2+1] - pts[i*2+1]};
            Vec2 next_dir = {pts[i*2] - pts[(i+1)*2], pts[i*2+1] - pts[(i+1)*2+1]};
            prev_dir = prev_dir.Unit();
            next_dir = next_dir.Unit();

            Vec2 prev_perp = prev_dir.Perp();
            Vec2 next_perp = next_dir.Perp();

            // miter = left 侧两个法向量的和
            Vec2 miter_left = prev_perp + next_perp;
            miter_left = miter_left.Unit();

            // miter 长度因子：1 / cos(夹角/2) = 1 / |prev_perp · miter_left|
            float cos_half = std::abs(prev_perp.Dot(miter_left));
            float miter_len = (cos_half > 1e-4f) ? (1.0f / cos_half) : 10.0f;
            miter_len = std::min(miter_len, 4.0f);  // 限制最大倍率

            normals[i].left  = miter_left * miter_len;
            normals[i].right = {-miter_left.x * miter_len, -miter_left.y * miter_len};
        }
    }

    // 生成 triangle strip
    // 策略：对每段输出 (left_i, right_i, left_{i+1}, right_{i+1})
    // 这构成一个连续 strip，没有额外三角形但 miter 已经处理了接缝
    for (size_t i = 0; i < n; ++i) {
        auto add_vert = [&](const Vec2& p) {
            out.push_back(p.x);
            out.push_back(p.y);
        };

        Vec2 cur = {pts[i*2], pts[i*2+1]};
        add_vert(cur + normals[i].left * half_w);
        add_vert(cur + normals[i].right * half_w);
    }

    return out;
}

}  // anonymous namespace

// ============================================================
// jpov::Renderer
// ============================================================

namespace jpov {

Renderer::Renderer() = default;

Renderer::~Renderer() {
    if (polyline2d_prog_) glDeleteProgram(polyline2d_prog_);
    if (stream_vbo_)      glDeleteBuffers(1, &stream_vbo_);
    if (fbo_) {
        glDeleteFramebuffers(1, &fbo_);
        glDeleteTextures(1, &color_tex_);
    }
}

void Renderer::CreateFBO(int width, int height) {
    CHECK_GT(width, 0);
    CHECK_GT(height, 0);
    CHECK_LE(width, kMaxFboWidth);
    CHECK_LE(height, kMaxFboHeight);

    fbo_width_  = width;
    fbo_height_ = height;

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
        << "FBO creation failed, status=" << status;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    LOG(INFO) << "Renderer: FBO ready (" << width << "x" << height << ")";
}

void Renderer::CompileShaders() {
    polyline2d_prog_ = BuildPolyline2DProgram();
    CHECK_NE(polyline2d_prog_, 0u) << "Polyline2D shader compilation failed";
    LOG(INFO) << "Renderer: shaders compiled (prog=" << polyline2d_prog_ << ")";
}

void Renderer::CreateStreamVBO() {
    size_t buf_size =
        static_cast<size_t>(kMaxStreamVertices) * 2 * sizeof(float);

    glGenBuffers(1, &stream_vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
    glBufferData(GL_ARRAY_BUFFER, buf_size, nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    LOG(INFO) << "Renderer: stream VBO ready (" << (buf_size / 1024) << " KB)";
}

void Renderer::Init(int window_width, int window_height) {
    CHECK_GT(window_width, 0);
    CHECK_GT(window_height, 0);

    CreateFBO(kMaxFboWidth, kMaxFboHeight);
    CompileShaders();
    CreateStreamVBO();
}

void Renderer::BeginFrame() {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, fbo_width_, fbo_height_);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

void Renderer::Render(const RenderCommandList& cmds, const Camera& camera,
                       const WindowInfo& winfo) {
    (void)camera;
    (void)winfo;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    for (const auto& [type, idx] : cmds.order) {
        switch (type) {
            case DrawCommandType::kPolyline2D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.polyline2d.size()));
                DrawPolyline2D(cmds.polyline2d[idx]);
                break;
            }
            default:
                break;
        }
    }
}

void Renderer::Present(GLFWwindow* window) {
    int fb_w, fb_h;
    glfwGetFramebufferSize(window, &fb_w, &fb_h);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, fbo_width_, fbo_height_,
                      0, 0, fb_w, fb_h,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Renderer::CapturePixels(uint8_t* out_pixels /*output*/,
                              int width, int height) {
    CHECK_NOTNULL(out_pixels);
    CHECK_GT(width, 0);
    CHECK_GT(height, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, out_pixels);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Renderer::DrawPolyline2D(const Polyline2DCommand& cmd) {
    if (cmd.vertices.size() < 2) return;

    // ---- CPU 端展开带宽度的 polyline（miter join） ----
    // 输入：cmd.vertices（Vec2f 数组）
    // 输出：flat TS 顶点数组（float array, 每两个 float 一个顶点）

    size_t n = cmd.vertices.size();
    std::vector<float> raw_pts;
    raw_pts.reserve(n * 2);
    for (const auto& v : cmd.vertices) {
        raw_pts.push_back(v.x());
        raw_pts.push_back(v.y());
    }

    std::vector<float> strip = BuildThickPolyline(
        raw_pts.data(), n, cmd.line_width);

    if (strip.size() < 4) return;  // 至少 2 个顶点

    // ---- 上传到 VBO 并绘制 ----
    glUseProgram(polyline2d_prog_);

    // uniform: 窗口大小（像素）
    glUniform2f(glGetUniformLocation(polyline2d_prog_, "uWindowSize"),
                static_cast<float>(fbo_width_),
                static_cast<float>(fbo_height_));

    // uniform: 颜色
    glUniform4f(glGetUniformLocation(polyline2d_prog_, "uColor"),
                cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);

    // orphan + subdata
    size_t upload_bytes = strip.size() * sizeof(float);
    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
    glBufferData(GL_ARRAY_BUFFER, upload_bytes, nullptr, GL_DYNAMIC_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, upload_bytes, strip.data());

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);

    // 用 GL_TRIANGLE_STRIP 绘制
    glDrawArrays(GL_TRIANGLE_STRIP, 0, static_cast<GLsizei>(strip.size() / 2));

    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

}  // namespace jpov
