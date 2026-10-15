// JPOV VolumeManager —— 3D 体积纹理（`.jpovvol`）的生命周期管理
//
// 定位：与 `TextureManager`（2D 纹理）**并列**的独立管理器，管理 **JPVOL2 体积** →
// GL `TEXTURE_2D_ARRAY`（层=帧）的加载、去重、查询、释放。作为 Renderer 的内部组件，
// 不直接暴露给用户（用户经 `JPOV::RegisterVolumeTexture` 间接使用）。
//
// 与 TextureManager 的差异：
//   · 2D 纹理按 (路径+选项) 去重、只记 w/h；本管理器按**路径**去重，另记 (nx,ny,nz,nframes)。
//   · 解析不可信文件 → **对维数/帧数/通道/格式做硬校验**（越界/畸形直接拒绝，见 LoadFromFile）。
//
// 体积 id：uint32_t，由本管理器分配；内部映射到 GL 纹理对象。析构时释放全部自有纹理。

#ifndef JPOV_VOLUME_MANAGER_H_
#define JPOV_VOLUME_MANAGER_H_

#include <cstdint>
#include <string>
#include <unordered_map>

#include "tools/jpov/interface/id_allocator.h"

namespace jpov {

// 一个已加载体积的元数据。
struct VolumeInfo {
    unsigned int gl_tex = 0;   // GL_TEXTURE_2D_ARRAY（层=帧）
    int nx = 0, ny = 0, nz = 0;
    int nframes = 0;
};

class VolumeManager {
public:
    VolumeManager() = default;
    ~VolumeManager();

    VolumeManager(const VolumeManager&) = delete;
    VolumeManager& operator=(const VolumeManager&) = delete;

    // LoadFromFile: 从 `.jpovvol`（JPVOL2）加载体积到 GPU（GL_TEXTURE_2D_ARRAY，层=帧）。
    //
    // 同一路径只加载一次（重复调用返回相同 id）。文件缺失/解析失败/维数越界 → 返回 0。
    // Pre-condition: GL context 已激活；path 非空。
    uint32_t LoadFromFile(const std::string& path);

    // 查询某个 id 的 GL 纹理（不存在 → 0）。
    unsigned int GetGLTexture(uint32_t id) const;
    // 查询某个 id 的维数/帧数（不存在 → false）。
    bool GetInfo(uint32_t id, VolumeInfo* out /*output*/) const;
    int FrameCount(uint32_t id) const;

    // 释放一个体积（glDeleteTextures + 移除记录）。id 不存在 → 静默忽略。
    void Release(uint32_t id);

private:
    IdAllocator id_alloc_;
    std::unordered_map<uint32_t, VolumeInfo> entries_;
    std::unordered_map<std::string, uint32_t> path_to_id_;
};

}  // namespace jpov

#endif  // JPOV_VOLUME_MANAGER_H_
