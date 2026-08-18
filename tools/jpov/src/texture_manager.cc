// JPOV TextureManager 实现
//
// GPU 纹理管理：PNG 加载 → GPU 上传 → 去重缓存。

// 需要 glGenerateMipmap 等 GL 扩展函数原型，必须在任何 GL 头 include 之前定义。
// MinGW 路径：原型经 gl_loader.h 的别名宏替换为运行时加载函数指针。
#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/texture_manager.h"

// GL 头文件必须最先 include（在 MinGW #define 宏替换之前），
// 否则 GL 常量（GL_TEXTURE_2D 等）在 MinGW 路径下不可见。
// 顺序：GL 常量声明 → 再 #define 函数名映射
#ifdef _WIN32
#include <GL/gl.h>
#else
#include <GL/gl.h>
#endif

#ifdef _WIN32
// MinGW: windows.h 定义 ERROR 宏与 glog 冲突，必须在 glog 之前 suppress
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
#include "third_party/gl_loader-mingw/gl_loader.h"

// 宏别名：替换代码中的 GL 函数名为 gl_loader 函数指针
// 注意：#define 必须在 #include <GL/gl.h> 之后，
// 否则会影响 gl.h 中的函数声明。
#define glGenTextures    gl_GenTextures
#define glDeleteTextures gl_DeleteTextures
#define glBindTexture    gl_BindTexture
#define glTexImage2D     gl_TexImage2D
#define glTexParameteri  gl_TexParameteri
#define glGenerateMipmap gl_GenerateMipmap

// MinGW 的 GL/gl.h 可能缺少这些 GL 常量（旧版 OpenGL 1.1）
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_REPEAT
#define GL_REPEAT 0x2901
#endif
#ifndef GL_LINEAR_MIPMAP_LINEAR
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#endif
#endif

#include "third_party/stb/stb_image.h"
#include <glog/logging.h>

namespace jpov {

TextureManager::~TextureManager() {
    for (auto& [id, entry] : entries_) {
        if (entry.owned && entry.gl_tex) {
            glDeleteTextures(1, &entry.gl_tex);
        }
    }
    entries_.clear();
    path_to_id_.clear();
    gl_tex_to_id_.clear();
}

uint32_t TextureManager::LoadFromFile(const std::string& path,
                                      const TextureOptions& opts) {
    CHECK(!path.empty()) << "TextureManager::LoadFromFile: path is empty";

    // 去重：同一「路径+选项」已加载过
    const std::string key = MakePathKey(path, opts);
    auto it = path_to_id_.find(key);
    if (it != path_to_id_.end()) {
        return it->second;
    }

    // stb_image 解码
    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* data = stbi_load(path.c_str(), &width, &height, &channels, 4);
    CHECK(data != nullptr)
        << "TextureManager::LoadFromFile: failed to load image: " << path
        << " — " << stbi_failure_reason();
    CHECK_GT(width, 0);
    CHECK_GT(height, 0);

    // 上传到 GPU
    GLuint gl_tex = 0;
    glGenTextures(1, &gl_tex);
    CHECK_NE(gl_tex, 0u);
    glBindTexture(GL_TEXTURE_2D, gl_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, data);

    // 采样过滤器：mipmap 开 → 三线性（大透视平铺面防摩尔纹/闪烁）；否则单 mip。
    const GLint min_filter =
        opts.mipmap ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // 包裹模式：repeat 开 → GL_REPEAT（UV>1 周期重复平铺）；否则 CLAMP_TO_EDGE。
    const GLint wrap = opts.repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);

    // mipmap 开：在完整 mip0 上传后生成金字塔链。
    if (opts.mipmap) {
        glGenerateMipmap(GL_TEXTURE_2D);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    stbi_image_free(data);

    GLenum err = glGetError();
    CHECK_EQ(err, GL_NO_ERROR)
        << "TextureManager::LoadFromFile: GL error after upload, code=" << err;

    uint32_t id = next_id_++;
    entries_[id] = {gl_tex, width, height, /*owned=*/true, opts};
    path_to_id_[key] = id;

    LOG(INFO) << "TextureManager: loaded \"" << path << "\" "
              << width << "x" << height << " → id=" << id
              << " gl_tex=" << gl_tex
              << " (mipmap=" << (opts.mipmap ? "on" : "off")
              << ", repeat=" << (opts.repeat ? "on" : "off") << ")";

    return id;
}

uint32_t TextureManager::Register(uint32_t gl_tex, int width, int height) {
    CHECK_NE(gl_tex, 0u);
    CHECK_GT(width, 0);
    CHECK_GT(height, 0);

    // 去重：同一 gl_tex 已注册过
    auto it = gl_tex_to_id_.find(gl_tex);
    if (it != gl_tex_to_id_.end()) {
        return it->second;
    }

    uint32_t id = next_id_++;
    entries_[id] = {gl_tex, width, height, /*owned=*/false, /*opts=*/{}};
    gl_tex_to_id_[gl_tex] = id;

    return id;
}

bool TextureManager::GetSize(uint32_t id, int* width, int* height) const {
    auto it = entries_.find(id);
    if (it == entries_.end()) {
        return false;
    }
    if (width) {
        *width = it->second.width;
    }
    if (height) {
        *height = it->second.height;
    }
    return true;
}

uint32_t TextureManager::GetGLTexture(uint32_t id) const {
    auto it = entries_.find(id);
    if (it == entries_.end()) {
        return 0;
    }
    return it->second.gl_tex;
}

void TextureManager::Release(uint32_t id) {
    auto it = entries_.find(id);
    if (it == entries_.end()) {
        return;
    }

    Entry& entry = it->second;

    // 如果是 owned 纹理，删除 GL 资源
    if (entry.owned && entry.gl_tex) {
        glDeleteTextures(1, &entry.gl_tex);
    }

    // 清理反向索引
    for (auto pi = path_to_id_.begin(); pi != path_to_id_.end(); ++pi) {
        if (pi->second == id) {
            path_to_id_.erase(pi);
            break;
        }
    }
    if (entry.gl_tex) {
        gl_tex_to_id_.erase(entry.gl_tex);
    }

    entries_.erase(it);
}

std::string TextureManager::MakePathKey(const std::string& path,
                                        const TextureOptions& opts) {
    // 同一路径不同选项 → 不同纹理，key 附 mip/repeat 位。
    return path + "#mip=" + (opts.mipmap ? "1" : "0") +
           ";rep=" + (opts.repeat ? "1" : "0");
}

}  // namespace jpov
