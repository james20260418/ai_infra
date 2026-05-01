// JPOV Renderer 实现
//
// OpenGL 资源全生命周期管理与绘制调度。
// 所有 GL 细节沉在此文件内部。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/renderer.h"

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

const char* kGs = R"glsl(
#version 330 core
layout(lines) in;
layout(triangle_strip, max_vertices = 4) out;

uniform float uLineWidthNDC;

void main() {
    vec2 p0 = gl_in[0].gl_Position.xy;
    vec2 p1 = gl_in[1].gl_Position.xy;

    vec2 dir = normalize(p1 - p0);
    vec2 perp = vec2(-dir.y, dir.x);

    vec2 offset = perp * uLineWidthNDC * 0.5;

    gl_Position = vec4(p0 - offset, 0.0, 1.0); EmitVertex();
    gl_Position = vec4(p0 + offset, 0.0, 1.0); EmitVertex();
    gl_Position = vec4(p1 - offset, 0.0, 1.0); EmitVertex();
    gl_Position = vec4(p1 + offset, 0.0, 1.0); EmitVertex();
    EndPrimitive();
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
        else if (type == GL_GEOMETRY_SHADER) type_name = "GS";
        else if (type == GL_FRAGMENT_SHADER) type_name = "FS";
        LOG(FATAL) << "Shader compile error [" << type_name << "]: " << log;
    }
    return shader;
}

unsigned int LinkProgram(unsigned int vs, unsigned int gs, unsigned int fs) {
    unsigned int prog = glCreateProgram();
    glAttachShader(prog, vs);
    if (gs) glAttachShader(prog, gs);
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
    if (gs) glDeleteShader(gs);
    glDeleteShader(fs);
    return prog;
}

unsigned int BuildPolyline2DProgram() {
    unsigned int vs = CompileShader(GL_VERTEX_SHADER, kVs);
    unsigned int gs = CompileShader(GL_GEOMETRY_SHADER, kGs);
    unsigned int fs = CompileShader(GL_FRAGMENT_SHADER, kFs);
    return LinkProgram(vs, gs, fs);
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

    // orphan 策略：每帧 glBufferData(nullptr) + glBufferSubData
    // 兼容性好，不需要 GL 4.4+ persistent mapping
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
    (void)camera;  // MVP 阶段只做 2D
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

    glUseProgram(polyline2d_prog_);

    // uniform: 窗口大小（像素）
    glUniform2f(glGetUniformLocation(polyline2d_prog_, "uWindowSize"),
                static_cast<float>(fbo_width_),
                static_cast<float>(fbo_height_));

    // uniform: NDC 线宽
    float lw_ndc = 2.0f * cmd.line_width / static_cast<float>(fbo_height_);
    glUniform1f(glGetUniformLocation(polyline2d_prog_, "uLineWidthNDC"),
                lw_ndc);

    // uniform: 颜色
    glUniform4f(glGetUniformLocation(polyline2d_prog_, "uColor"),
                cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);

    // ---- 展开为线段对 ----
    size_t n = cmd.vertices.size();
    size_t seg_count = n - 1;
    std::vector<float> flat;
    flat.reserve(seg_count * 4);

    for (size_t i = 0; i < seg_count; ++i) {
        flat.push_back(cmd.vertices[i].x());
        flat.push_back(cmd.vertices[i].y());
        flat.push_back(cmd.vertices[i + 1].x());
        flat.push_back(cmd.vertices[i + 1].y());
    }

    // orphan + subdata
    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
    size_t upload_bytes = flat.size() * sizeof(float);
    glBufferData(GL_ARRAY_BUFFER, upload_bytes, nullptr, GL_DYNAMIC_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, upload_bytes, flat.data());

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);

    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(flat.size() / 2));

    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

}  // namespace jpov
