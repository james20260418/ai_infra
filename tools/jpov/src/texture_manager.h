// JPOV TextureManager — GPU 纹理生命周期管理
//
// 管理 PNG 图片 → GPU 纹理的加载、去重缓存、释放。
// 作为 Renderer 的内部组件，不直接暴露给用户。
// 用户通过 JPOV::RegisterTexture / JPOV::ReleaseTexture 间接使用。
//
// 纹理 ID:
//   - 外部纹理 ID 为 uint32_t，由 TextureManager 分配。
//   - 内部映射到 GL 纹理对象（GLuint）。
//   - 纹理 ID 在 TextureManager 析构时全部释放。
//
// 去重:
//   - LoadFromFile 对同一绝对路径只加载一次，返回相同 ID。
//   - Register 直接注册现有 GL 纹理，不查重（调用者保证不重复注册）。

#ifndef JPOV_TEXTURE_MANAGER_H_
#define JPOV_TEXTURE_MANAGER_H_

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace jpov {

class TextureManager {
public:
    TextureManager() = default;
    ~TextureManager();

    TextureManager(const TextureManager&) = delete;
    TextureManager& operator=(const TextureManager&) = delete;

    // LoadFromFile: 从 PNG 文件加载纹理到 GPU。
    //
    // 使用 stb_image 解码为 RGBA 后调用 glTexImage2D 上传。
    // 同一绝对路径只加载一次，后续调用返回相同 ID（去重）。
    // 加载失败（文件不存在 / 解码失败）→ LOG(FATAL) crash。
    //
    // Pre-condition: GL context 已激活
    // Pre-condition: path 非空
    uint32_t LoadFromFile(const std::string& path);

    // Register: 直接注册已有 GL 纹理。
    //
    // 调用者自行管理 GL 纹理生命周期（创建/释放）。
    // TextureManager 只记录尺寸信息，析构时不 delete 此纹理。
    // 同一 gl_tex 重复注册 → 返回已有 ID（按 gl_tex 查重）。
    //
    // Pre-condition: gl_tex 非 0
    // Pre-condition: width > 0, height > 0
    uint32_t Register(uint32_t gl_tex, int width, int height);

    // GetSize: 查询纹理尺寸。
    //
    // 返回 true 表示纹理已注册，width/height 被填充。
    // 返回 false 表示纹理 ID 不存在。
    bool GetSize(uint32_t id, int* width, int* height) const;

    // GetGLTexture: 获取纹理 ID 对应的 GL 纹理对象。
    //
    // 返回 0 表示纹理 ID 不存在。
    uint32_t GetGLTexture(uint32_t id) const;

    // Release: 释放纹理。
    //
    // 对于 LoadFromFile 加载的纹理：glDeleteTextures + 移除缓存。
    // 对于 Register 注册的外部纹理：仅移除记录，不 delete GL 纹理。
    //
    // 纹理 ID 不存在 → 静默忽略（允许重复释放）。
    void Release(uint32_t id);

private:
    struct Entry {
        uint32_t gl_tex;
        int width;
        int height;
        bool owned;  // true = TextureManager 负责 glDeleteTextures
    };

    // id counter（递增分配）
    uint32_t next_id_ = 1;

    // id → Entry
    std::unordered_map<uint32_t, Entry> entries_;

    // 文件路径 → id（LoadFromFile 去重）
    std::unordered_map<std::string, uint32_t> path_to_id_;

    // gl_tex → id（Register 去重）
    std::unordered_map<uint32_t, uint32_t> gl_tex_to_id_;
};

}  // namespace jpov

#endif  // JPOV_TEXTURE_MANAGER_H_
