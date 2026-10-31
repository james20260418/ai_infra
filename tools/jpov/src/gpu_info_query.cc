// JPOV GPU 信息查询实现

#include "tools/jpov/src/gpu_info_query.h"

#include <string>

// GL 头文件必须最先 include。**两个平台都要 <GL/glext.h>**：MinGW 的 <GL/gl.h>
// 只到 1.1，GL_SHADING_LANGUAGE_VERSION / GL_MAX_VERTEX_ATTRIBS /
// GL_MAX_COLOR_ATTACHMENTS 这些枚举都在 glext 里（与 volume_manager.cc 同款）。
#include <GL/gl.h>
#include <GL/glext.h>
#ifdef _WIN32
// MinGW: windows.h 定义 ERROR 宏与 glog 冲突，必须在 glog 之前 suppress
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
#include "third_party/gl_loader-mingw/gl_loader.h"
#endif

namespace jpov {
namespace {

// 读一条 glGetString；返回空串表示查询失败（无 context / 未定义）。
std::string ReadGlString(GLenum name) {
    const GLubyte* raw = glGetString(name);
    if (raw == nullptr) {
        return std::string();
    }
    return std::string(reinterpret_cast<const char*>(raw));
}

// 读一条 glGetIntegerv；查询失败（GL 不置初值）时返回 0。
int ReadGlInt(GLenum pname) {
    GLint value = 0;
    glGetIntegerv(pname, &value);
    return static_cast<int>(value);
}

}  // namespace

GpuInfo QueryGpuInfo() {
    GpuInfo info;
    info.renderer     = ReadGlString(GL_RENDERER);
    info.vendor       = ReadGlString(GL_VENDOR);
    info.version      = ReadGlString(GL_VERSION);
    info.glsl_version = ReadGlString(GL_SHADING_LANGUAGE_VERSION);
    // 分类规则是纯字符串逻辑，放在 interface/gpu_info.cc（无 GL 依赖、可单测）。
    info.kind = ClassifyGpuRenderer(info.renderer);

    info.max_texture_size      = ReadGlInt(GL_MAX_TEXTURE_SIZE);
    info.max_vertex_attribs    = ReadGlInt(GL_MAX_VERTEX_ATTRIBS);
    info.max_color_attachments = ReadGlInt(GL_MAX_COLOR_ATTACHMENTS);
    return info;
}

}  // namespace jpov
