// JPOV FireFog — 共享「3D 火纹理」体积文件读取器（GL-free，可单测）。
//
// 支持两种版本，都是「离线烘 → 引擎运行期只读」的简单稠密格式（重活留在离线转换）：
//
//   JPVOL1（单帧；历史版本）
//     offset 0  : char   magic[8] = "JPVOL1\0\0"
//     offset 8  : uint32 version(=1)
//     offset 12 : uint32 nx, ny, nz
//     offset 24 : uint32 channels        (2: R=density, G=temperature)
//     offset 28 : uint32 dtype           (0 = float32)
//     offset 32 : 数据 = nx*ny*nz*channels 个 float32
//
//   JPVOL2（帧序列；Mantaflow 火仿真输出）
//     offset 0  : char   magic[8] = "JPVOL2\0\0"
//     offset 8  : uint32 version(=2)
//     offset 12 : uint32 nx, ny, nz
//     offset 24 : uint32 nframes         (>= 1)
//     offset 28 : uint32 channels        (2: R=density, G=temperature)
//     offset 32 : uint32 dtype           (0 = float32, 1 = uint8 归一化)
//     offset 36 : uint32 reserved
//     offset 40 : 数据 = nframes*nx*ny*nz*channels（帧内 x 最快→y→z；帧间顺序）
//
// 体素/帧布局都满足「逐帧一整块连续、块内 x 最快→y→z、通道交错」⇒ 可直接喂
// glTexImage3D(GL_TEXTURE_2D_ARRAY)：每个 z 切片 = 一张 (nx, ny) 图层，图层序 = frame*nz + z。
//
// 通道语义（归一化 [0,1]）：R=density（→ 消光 σ=density_scale·R）；G=temperature（→ 自发光=color·G·temp_scale）。
//
// 失败一律 LOG(FATAL)（配置错误，不静默回退）。

#ifndef JPOV_SRC_FIRE_FOG_FIRE_VOLUME_H_
#define JPOV_SRC_FIRE_FOG_FIRE_VOLUME_H_

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <glog/logging.h>

namespace jpov {

// 稠密体积（单帧或帧序列）。数据存于 data（float32）或 bytes（uint8，归一化 [0,1]）之一。
struct FireVolume {
    int nx = 0;
    int ny = 0;
    int nz = 0;
    int nframes = 1;
    int channels = 0;
    bool uint8 = false;                 // true → 用 bytes（uint8）；false → 用 data（float32）
    std::vector<float> data;            // uint8==false 时有效；长度 = nframes*nx*ny*nz*channels
    std::vector<unsigned char> bytes;   // uint8==true 时有效；值域 [0,255] 对应 [0,1]

    bool empty() const { return uint8 ? bytes.empty() : data.empty(); }
};

// 读取 .jpovvol（JPVOL1/JPVOL2）。任何格式/IO 错误 → LOG(FATAL)。
// Pre-condition: path 非空
inline FireVolume LoadFireVolumeFile(const std::string& path) {
    CHECK(!path.empty()) << "LoadFireVolumeFile: path 为空";

    std::ifstream in(path, std::ios::binary);
    CHECK(in.is_open()) << "LoadFireVolumeFile: 打不开文件 " << path;

    unsigned char magic[8];
    in.read(reinterpret_cast<char*>(magic), 8);
    CHECK(in.gcount() == 8) << "LoadFireVolumeFile: 读不到 magic " << path;

    static const char kMagic1[8] = {'J', 'P', 'V', 'O', 'L', '1', '\0', '\0'};
    static const char kMagic2[8] = {'J', 'P', 'V', 'O', 'L', '2', '\0', '\0'};
    const bool v2 = (std::memcmp(magic, kMagic2, 8) == 0);
    const bool v1 = (std::memcmp(magic, kMagic1, 8) == 0);
    CHECK(v1 || v2) << "LoadFireVolumeFile: magic 不是 JPVOL1/JPVOL2 " << path;

    const size_t hdr_size = v2 ? 40 : 32;
    unsigned char hdr[40];
    std::memcpy(hdr, magic, 8);
    in.read(reinterpret_cast<char*>(hdr + 8), static_cast<std::streamsize>(hdr_size - 8));
    CHECK_EQ(in.gcount(), static_cast<std::streamsize>(hdr_size - 8))
        << "LoadFireVolumeFile: 头不完整 " << path;

    auto rd = [&](size_t off) -> uint32_t {
        uint32_t v = 0;
        std::memcpy(&v, hdr + off, 4);
        return v;
    };
    const uint32_t version = rd(8);
    const uint32_t nx = rd(12);
    const uint32_t ny = rd(16);
    const uint32_t nz = rd(20);
    const uint32_t nframes = v2 ? rd(24) : 1u;
    const uint32_t channels = v2 ? rd(28) : rd(24);
    const uint32_t dtype = v2 ? rd(32) : rd(28);

    CHECK_EQ(version, v2 ? 2u : 1u) << "LoadFireVolumeFile: 版本不符 " << version;
    CHECK_GT(nx, 0u) << "LoadFireVolumeFile: nx 必须 > 0";
    CHECK_GT(ny, 0u) << "LoadFireVolumeFile: ny 必须 > 0";
    CHECK_GT(nz, 0u) << "LoadFireVolumeFile: nz 必须 > 0";
    CHECK_GE(nframes, 1u) << "LoadFireVolumeFile: nframes 必须 >= 1";
    CHECK_GE(channels, 1u) << "LoadFireVolumeFile: channels 必须 >= 1";
    CHECK_LE(channels, 4u) << "LoadFireVolumeFile: channels 必须 <= 4";
    CHECK_LE(dtype, 1u) << "LoadFireVolumeFile: dtype 只支持 0/1，实际 " << dtype;

    FireVolume vol;
    vol.nx = static_cast<int>(nx);
    vol.ny = static_cast<int>(ny);
    vol.nz = static_cast<int>(nz);
    vol.nframes = static_cast<int>(nframes);
    vol.channels = static_cast<int>(channels);
    vol.uint8 = (dtype == 1);

    const uint64_t count = static_cast<uint64_t>(nx) * ny * nz * nframes * channels;
    if (vol.uint8) {
        vol.bytes.resize(static_cast<size_t>(count));
        in.read(reinterpret_cast<char*>(vol.bytes.data()), static_cast<std::streamsize>(count));
        CHECK_EQ(in.gcount(), static_cast<std::streamsize>(count))
            << "LoadFireVolumeFile: uint8 数据字节数不符 " << path;
    } else {
        vol.data.resize(static_cast<size_t>(count));
        in.read(reinterpret_cast<char*>(vol.data.data()),
                static_cast<std::streamsize>(count * sizeof(float)));
        CHECK_EQ(in.gcount(), static_cast<std::streamsize>(count * sizeof(float)))
            << "LoadFireVolumeFile: float 数据字节数不符 " << path;
    }
    return vol;
}

}  // namespace jpov

#endif  // JPOV_SRC_FIRE_FOG_FIRE_VOLUME_H_
