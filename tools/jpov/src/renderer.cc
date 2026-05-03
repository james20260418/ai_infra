// JPOV Renderer 实现
// FBO 动态调整，坐标以渲染分辨率为空间。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/renderer.h"

#include <algorithm>
#include <cstdio>
#include <vector>

#include <GLFW/glfw3.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include <glog/logging.h>

namespace {

const char* kVs = R"glsl(
#version 330 core
layout(location = 0) in vec2 aPos;
uniform vec2 uResolution;

void main() {
    vec2 ndc = (aPos / uResolution) * 2.0 - 1.0;
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

unsigned int CompileShader(GLenum type, const char* source) {
    unsigned int shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        const char* tn = (type == GL_VERTEX_SHADER) ? "VS" : "FS";
        LOG(FATAL) << "Shader compile error [" << tn << "]: " << log;
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

}  // anonymous namespace

namespace jpov {

const Color kColorRed         = {1.0f, 0.0f, 0.0f, 1.0f};
const Color kColorGreen       = {0.0f, 1.0f, 0.0f, 1.0f};
const Color kColorBlue        = {0.0f, 0.0f, 1.0f, 1.0f};
const Color kColorWhite       = {1.0f, 1.0f, 1.0f, 1.0f};
const Color kColorBlack       = {0.0f, 0.0f, 0.0f, 1.0f};
const Color kColorTransparent = {0.0f, 0.0f, 0.0f, 0.0f};

// === RenderCommandList methods ===

void RenderCommandList::Clear() {
    polyline2d.clear();
    rect2d.clear();
    circle2d.clear();
    text2d.clear();
    line3d.clear();
    triangle3d.clear();
    text3d.clear();
    order.clear();
    // render_width/render_height 不清零
}

void RenderCommandList::DrawPolyline(const std::vector<Vec2f>& vertices,
                                     const Color& color, float line_width) {
    CHECK_GT(line_width, 0.0f);
    int idx = static_cast<int>(polyline2d.size());
    polyline2d.push_back({vertices, color, line_width});
    order.emplace_back(DrawCommandType::kPolyline2D, idx);
}

void RenderCommandList::DrawRect(const Vec2f& pos, const Vec2f& size,
                                  const Color& color) {
    int idx = static_cast<int>(rect2d.size());
    rect2d.push_back({pos, size, color});
    order.emplace_back(DrawCommandType::kRect2D, idx);
}

void RenderCommandList::DrawCircle(const Vec2f& center, float radius,
                                    const Color& color) {
    CHECK_GT(radius, 0.0f);
    int idx = static_cast<int>(circle2d.size());
    circle2d.push_back({center, radius, color});
    order.emplace_back(DrawCommandType::kCircle2D, idx);
}

void RenderCommandList::DrawText(const std::string& text, const Vec2f& pos,
                                  float font_size, const Color& color) {
    CHECK_GT(font_size, 0.0f);
    int idx = static_cast<int>(text2d.size());
    text2d.push_back({text, pos, font_size, color});
    order.emplace_back(DrawCommandType::kText2D, idx);
}

void RenderCommandList::DrawLine3D(const Vec3f& p1, const Vec3f& p2,
                                    const Color& color, float width) {
    CHECK_GT(width, 0.0f);
    int idx = static_cast<int>(line3d.size());
    line3d.push_back({p1, p2, color, width});
    order.emplace_back(DrawCommandType::kLine3D, idx);
}

void RenderCommandList::DrawTriangle3D(const Vec3f& p1, const Vec3f& p2,
                                        const Vec3f& p3, const Color& color) {
    int idx = static_cast<int>(triangle3d.size());
    triangle3d.push_back({p1, p2, p3, color});
    order.emplace_back(DrawCommandType::kTriangle3D, idx);
}

void RenderCommandList::DrawText3D(const std::string& text, const Vec3f& pos,
                                    float font_size, const Color& color) {
    CHECK_GT(font_size, 0.0f);
    int idx = static_cast<int>(text3d.size());
    text3d.push_back({text, pos, font_size, color});
    order.emplace_back(DrawCommandType::kText3D, idx);
}

// === Renderer ===

Renderer::Renderer() = default;

Renderer::~Renderer() {
    DestroyFBO();
    if (prog_)       glDeleteProgram(prog_);
    if (stream_vbo_) glDeleteBuffers(1, &stream_vbo_);
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
    LOG(INFO) << "Renderer: FBO " << width << "x" << height;
}

void Renderer::CompileShaders() {
    unsigned int vs = CompileShader(GL_VERTEX_SHADER, kVs);
    unsigned int fs = CompileShader(GL_FRAGMENT_SHADER, kFs);
    prog_ = LinkProgram(vs, fs);
    CHECK_NE(prog_, 0u);
}

void Renderer::CreateStreamVBO() {
    size_t buf = static_cast<size_t>(kMaxStreamVertices) * 2 * sizeof(float);
    glGenBuffers(1, &stream_vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
    glBufferData(GL_ARRAY_BUFFER, buf, nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void Renderer::Init() {
    CompileShaders();
    CreateStreamVBO();
}

void Renderer::BeginFrame(int render_w, int render_h) {
    EnsureFBO(render_w, render_h);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, render_w, render_h);
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
            case DrawCommandType::kRect2D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.rect2d.size()));
                DrawRect2D(cmds.rect2d[idx]);
                break;
            }
            default:
                break;
        }
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
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Renderer::DrawRect2D(const Rect2DCommand& cmd) {
    float verts[8];
    float x0 = cmd.pos.x();
    float y0 = cmd.pos.y();
    float x1 = x0 + cmd.size.x();
    float y1 = y0 + cmd.size.y();
    verts[0] = x0; verts[1] = y0;
    verts[2] = x1; verts[3] = y0;
    verts[4] = x1; verts[5] = y1;
    verts[6] = x0; verts[7] = y1;

    glUseProgram(prog_);
    glUniform2f(glGetUniformLocation(prog_, "uResolution"),
                static_cast<float>(fbo_w_),
                static_cast<float>(fbo_h_));
    glUniform4f(glGetUniformLocation(prog_, "uColor"),
                cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);

    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

}  // namespace jpov
