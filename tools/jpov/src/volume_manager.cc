// JPOV VolumeManager 实现。见 volume_manager.h。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/volume_manager.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

#include <GL/gl.h>
#include <GL/glext.h>

#ifdef _WIN32
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
#include "third_party/gl_loader-mingw/gl_loader.h"
#endif

#include <glog/logging.h>

namespace jpov {

namespace {

// JPVOL2 header（little-endian，40 字节，与 tools/jpov/genVolume/jpvol.py 对应）。
struct JpovVol2Header {
    char magic[8];        // "JPVOL2\0\0"
    uint32_t version;     // == 2
    uint32_t nx, ny, nz;
    uint32_t nframes;
    uint32_t channels;    // 2 = (R=火密度, G=温度)；3 = 再带 B=烟密度
    uint32_t dtype;       // 0=float32, 1=uint8(归一化)
    uint32_t reserved;
};

// ── 不可信输入硬边界（防畸形/恶意文件 → 巨额分配 / 整数溢出 / GL 越界）──
constexpr uint32_t kMaxDim = 2048;        // 单轴体素数上限
constexpr uint32_t kMaxFrames = 4096;     // 帧数上限
constexpr size_t kMaxTotalBytes = size_t{2} << 30;   // 数据区上限 2 GiB

}  // namespace

VolumeManager::~VolumeManager() {
    for (auto& kv : entries_) {
        if (kv.second.gl_tex != 0) {
            glDeleteTextures(1, &kv.second.gl_tex);
        }
        if (kv.second.gl_tex2 != 0) {
            glDeleteTextures(1, &kv.second.gl_tex2);
        }
    }
    entries_.clear();
    path_to_id_.clear();
}

uint32_t VolumeManager::LoadFromFile(const std::string& path) {
    const auto it = path_to_id_.find(path);
    if (it != path_to_id_.end()) {
        return it->second;
    }
    if (path.empty()) {
        LOG(WARNING) << "VolumeManager: 空路径";
        return 0;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) {
        LOG(WARNING) << "VolumeManager: 打不开 " << path;
        return 0;
    }
    JpovVol2Header h{};
    f.read(reinterpret_cast<char*>(&h), sizeof(h));
    if (!f.good() || std::memcmp(h.magic, "JPVOL2", 6) != 0) {
        LOG(WARNING) << "VolumeManager: 非 JPVOL2 文件 " << path;
        return 0;
    }
    // ── 硬校验（不可信输入）──
    if (h.version != 2u || (h.channels != 2u && h.channels != 3u) ||
        (h.dtype != 0u && h.dtype != 1u && h.dtype != 2u)) {
        LOG(WARNING) << "VolumeManager: 头非法 (ver=" << h.version << " ch=" << h.channels
                     << " dtype=" << h.dtype << ") " << path;
        return 0;
    }
    if (h.nx == 0u || h.ny == 0u || h.nz == 0u || h.nframes == 0u ||
        h.nx > kMaxDim || h.ny > kMaxDim || h.nz > kMaxDim || h.nframes > kMaxFrames) {
        LOG(WARNING) << "VolumeManager: 维数越界 (" << h.nx << "," << h.ny << "," << h.nz
                     << " x" << h.nframes << ") " << path;
        return 0;
    }
    // 用 size_t 逐乘，避免 uint32 溢出；再判总量上限。
    const size_t per_frame = static_cast<size_t>(h.nx) * static_cast<size_t>(h.ny) *
                             static_cast<size_t>(h.nz) * static_cast<size_t>(h.channels);
    const size_t bytes_per_frame = per_frame * (h.dtype == 1u ? 1u : (h.dtype == 2u ? 2u : 4u));
    const size_t total = bytes_per_frame * static_cast<size_t>(h.nframes);
    if (per_frame == 0 || bytes_per_frame == 0 || total == 0 || total > kMaxTotalBytes) {
        LOG(WARNING) << "VolumeManager: 数据量越界 (" << total << " B) " << path;
        return 0;
    }
    // nz*ny 会进 GLsizei（int）：确保不溢出 int。
    const size_t layer_h = static_cast<size_t>(h.ny) * static_cast<size_t>(h.nz);
    if (layer_h > 0x7fffffffULL || h.nx > 0x7fffffffULL || h.nframes > 0x7fffffffULL) {
        LOG(WARNING) << "VolumeManager: 维数超过 GLsizei " << path;
        return 0;
    }
    // 体积按 GL_TEXTURE_2D_ARRAY 上传：宽=nx、高=ny*nz（层=帧）。宽/高都受
    // GL_MAX_TEXTURE_SIZE 限制——超限时 glTexImage3D 失败、纹理不完整、采样全 0
    // （渲染成一片黑），**且不会报错**。
    //
    // 【临时 hack】层高超过上限时，把层高沿 (z+y) 行对半切成 2 张纹理（采样端在 shader
    //   里按 v 选块）。正式方案应改布局/格式，见 volume_manager.h 注释。
    GLint max_tex = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex);
    if (max_tex > 0 && h.nx > static_cast<uint32_t>(max_tex)) {
        LOG(WARNING) << "VolumeManager: 宽 " << h.nx << " 超过 GL_MAX_TEXTURE_SIZE="
                     << max_tex << "，无法上传：" << path;
        return 0;
    }
    size_t chunk0 = layer_h;   // 第一块的层高（= 总层高时不切分）
    bool split = false;
    if (max_tex > 0 && layer_h > static_cast<size_t>(max_tex)) {
        if (layer_h > 2u * static_cast<size_t>(max_tex)) {
            LOG(WARNING) << "VolumeManager: 层高 ny*nz=" << layer_h
                         << " 超过 2\xd7GL_MAX_TEXTURE_SIZE，临时 hack 只支持切 2 块：" << path;
            return 0;
        }
        split = true;
        chunk0 = (layer_h + 1u) / 2u;
        LOG(WARNING) << "VolumeManager: [hack] 层高 ny*nz=" << layer_h
                     << " 超过 GL_MAX_TEXTURE_SIZE=" << max_tex << "，切成两块 ("
                     << chunk0 << " + " << (layer_h - chunk0) << ")：" << path;
    }

    std::vector<unsigned char> buf(bytes_per_frame);
    f.seekg(sizeof(JpovVol2Header), std::ios::beg);

    const bool rgb = (h.channels == 3u);
    const GLenum internal = rgb ? (h.dtype == 1u ? GL_RGB8 : GL_RGB16F)
                                : (h.dtype == 1u ? GL_RG8 : GL_RG16F);
    const GLenum format = rgb ? GL_RGB : GL_RG;
    const GLenum type = (h.dtype == 1u) ? GL_UNSIGNED_BYTE
                                        : (h.dtype == 2u ? GL_HALF_FLOAT : GL_FLOAT);
    // 3 通道 = 每像素 3 字节，行不是 4 的倍数 ⇒ 必须把解包对齐设为 1，否则行错位。
    GLint prev_align = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    auto make_tex = [&](size_t ch_h) -> unsigned int {
        unsigned int t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D_ARRAY, t);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, internal, static_cast<GLsizei>(h.nx),
                     static_cast<GLsizei>(ch_h), static_cast<GLsizei>(h.nframes), 0,
                     format, type, nullptr);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        return t;
    };
    const size_t row_bytes = static_cast<size_t>(h.nx) * h.channels *
                             (h.dtype == 1u ? 1u : (h.dtype == 2u ? 2u : 4u));
    unsigned int tex = make_tex(chunk0);
    unsigned int tex2 = split ? make_tex(layer_h - chunk0) : 0;
    for (uint32_t fr = 0; fr < h.nframes; ++fr) {
        f.read(reinterpret_cast<char*>(buf.data()),
               static_cast<std::streamsize>(bytes_per_frame));
        if (!f.good()) {
            LOG(WARNING) << "VolumeManager: 数据不足（帧 " << fr << "）" << path;
            glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
            glDeleteTextures(1, &tex);
            if (tex2 != 0) { glDeleteTextures(1, &tex2); }
            glPixelStorei(GL_UNPACK_ALIGNMENT, prev_align);
            return 0;
        }
        glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, static_cast<GLint>(fr),
                        static_cast<GLsizei>(h.nx), static_cast<GLsizei>(chunk0),
                        1, format, type, buf.data());
        if (split) {
            glBindTexture(GL_TEXTURE_2D_ARRAY, tex2);
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, static_cast<GLint>(fr),
                            static_cast<GLsizei>(h.nx),
                            static_cast<GLsizei>(layer_h - chunk0), 1, format, type,
                            buf.data() + chunk0 * row_bytes);
        }
    }
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, prev_align);   // 复原全局解包对齐（不污染后续上传）

    const uint32_t id = id_alloc_.Acquire();
    if (id == 0u) {
        LOG(WARNING) << "VolumeManager: id 空间耗尽";
        glDeleteTextures(1, &tex);
        if (tex2 != 0) { glDeleteTextures(1, &tex2); }
        return 0;
    }
    VolumeInfo info;
    info.gl_tex = tex;
    info.nx = static_cast<int>(h.nx);
    info.ny = static_cast<int>(h.ny);
    info.nz = static_cast<int>(h.nz);
    info.nframes = static_cast<int>(h.nframes);
    info.channels = static_cast<int>(h.channels);
    info.gl_tex2 = tex2;
    info.chunk_split = split ? static_cast<float>(chunk0) / static_cast<float>(layer_h) : 1.0f;
    entries_[id] = info;
    path_to_id_[path] = id;
    LOG(INFO) << "[volume] loaded " << path << " dims=(" << h.nx << "," << h.ny << ","
              << h.nz << ") frames=" << h.nframes << " channels=" << h.channels
              << " dtype=" << h.dtype << " -> id " << id;
    return id;
}

unsigned int VolumeManager::GetGLTexture(uint32_t id) const {
    const auto it = entries_.find(id);
    return (it == entries_.end()) ? 0u : it->second.gl_tex;
}

bool VolumeManager::GetInfo(uint32_t id, VolumeInfo* out) const {
    CHECK(out != nullptr);
    const auto it = entries_.find(id);
    if (it == entries_.end()) {
        return false;
    }
    *out = it->second;
    return true;
}

int VolumeManager::FrameCount(uint32_t id) const {
    const auto it = entries_.find(id);
    return (it == entries_.end()) ? 0 : it->second.nframes;
}

void VolumeManager::Release(uint32_t id) {
    const auto it = entries_.find(id);
    if (it == entries_.end()) {
        return;
    }
    if (it->second.gl_tex != 0) {
        glDeleteTextures(1, &it->second.gl_tex);
    }
    if (it->second.gl_tex2 != 0) {
        glDeleteTextures(1, &it->second.gl_tex2);
    }
    for (auto pit = path_to_id_.begin(); pit != path_to_id_.end();) {
        if (pit->second == id) {
            pit = path_to_id_.erase(pit);
        } else {
            ++pit;
        }
    }
    entries_.erase(it);
    id_alloc_.Release(id);
}

}  // namespace jpov
