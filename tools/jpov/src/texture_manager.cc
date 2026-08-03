// JPOV TextureManager 实现
//
// GPU 纹理管理：PNG 加载 → GPU 上传 → 去重缓存。

#include "tools/jpov/src/texture_manager.h"

#ifdef _WIN32
// MinGW: windows.h 定义 ERROR 宏与 glog 冲突，必须在 glog 之前 suppress
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
#include "third_party/gl_loader-mingw/gl_loader.h"

// 宏别名：代码中用 glBindTexture 等，展开为 gl_loader 的函数指针调用
#define glGenTextures    gl_GenTextures
#define glDeleteTextures gl_DeleteTextures
#define glBindTexture    gl_BindTexture
#define glTexImage2D     gl_TexImage2D
#define glTexParameteri  gl_TexParameteri

// MinGW 的 gl_loader.h 应已提供 GL 类型和常量。
// 如果缺少 GL_CLAMP_TO_EDGE（旧版 OpenGL 1.1），手动定义。
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

#else
// Linux: 直接使用系统 GL
#include <GL/gl.h>
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

uint32_t TextureManager::LoadFromFile(const std::string& path) {
    CHECK(!path.empty()) << "TextureManager::LoadFromFile: path is empty";

    // 去重：同一路径已加载过
    auto it = path_to_id_.find(path);
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
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    stbi_image_free(data);

    GLenum err = glGetError();
    CHECK_EQ(err, GL_NO_ERROR)
        << "TextureManager::LoadFromFile: GL error after upload, code=" << err;

    uint32_t id = next_id_++;
    entries_[id] = {gl_tex, width, height, /*owned=*/true};
    path_to_id_[path] = id;

    LOG(INFO) << "TextureManager: loaded \"" << path << "\" "
              << width << "x" << height << " → id=" << id
              << " gl_tex=" << gl_tex;

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
    entries_[id] = {gl_tex, width, height, /*owned=*/false};
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

}  // namespace jpov
