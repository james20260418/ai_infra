// FireVolume（.jpovvol）读取器单测（GL-free）。
//
// 目的：把「离线转换器写出的字节」与「引擎运行期读入」钉在一起，防格式漂移：
//   - 正向：手工构造合法小体积（2×2×2×2ch）→ 写出 → 读回，逐字段/逐体素比对；
//   - 负向：magic 错 / 数据截断 → 必须 LOG(FATAL)（不静默返回空）。

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "tools/jpov/src/fire_fog/fire_volume.h"

namespace jpov {
namespace {

// 写一个 .jpovvol 文件（nx,ny,nz 个 float32 × channels，x 最快）。
void WriteVol(const std::string& path, uint32_t nx, uint32_t ny, uint32_t nz, uint32_t channels,
              const std::vector<float>& data, bool corrupt_magic = false) {
    std::ofstream out(path, std::ios::binary);
    unsigned char hdr[32];
    std::memset(hdr, 0, sizeof(hdr));
    const char magic[8] = {'J', 'P', 'V', 'O', 'L', '1', '\0', '\0'};
    std::memcpy(hdr, magic, 8);
    if (corrupt_magic) {
        hdr[0] = 'X';
    }
    auto wr = [&](size_t off, uint32_t v) { std::memcpy(hdr + off, &v, 4); };
    wr(8, 1);
    wr(12, nx);
    wr(16, ny);
    wr(20, nz);
    wr(24, channels);
    wr(28, 0);
    out.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size() * sizeof(float)));
}

std::string TempPath(const char* name) {
    return std::string("/tmp/jpov_fire_volume_test_") + name;
}

TEST(FireVolume, RoundTripPreservesDimsAndData) {
    const std::string path = TempPath("ok.jpovvol");
    // 2×2×2，2 通道（r=density, g=temp）；体素值取可辨识序列。
    std::vector<float> data(2 * 2 * 2 * 2);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<float>(i) * 0.5f;
    }
    WriteVol(path, 2, 2, 2, 2, data);

    const FireVolume vol = LoadFireVolumeFile(path);
    EXPECT_EQ(vol.nx, 2);
    EXPECT_EQ(vol.ny, 2);
    EXPECT_EQ(vol.nz, 2);
    EXPECT_EQ(vol.channels, 2);
    ASSERT_EQ(vol.data.size(), data.size());
    for (size_t i = 0; i < data.size(); ++i) {
        EXPECT_FLOAT_EQ(vol.data[i], data[i]) << "体素项 " << i << " 不一致";
    }
    std::remove(path.c_str());
}

TEST(FireVolume, SingleChannelSupported) {
    const std::string path = TempPath("one_ch.jpovvol");
    std::vector<float> data = {0.1f, 0.2f, 0.3f, 0.4f};   // 2×2×1×1ch
    WriteVol(path, 2, 2, 1, 1, data);
    const FireVolume vol = LoadFireVolumeFile(path);
    EXPECT_EQ(vol.channels, 1);
    EXPECT_EQ(vol.data.size(), 4u);
    EXPECT_FLOAT_EQ(vol.data[2], 0.3f);
    std::remove(path.c_str());
}

// 负向：magic 不符 → 必须 crash（不是静默返回空）。
TEST(FireVolume, BadMagicDies) {
    const std::string path = TempPath("bad_magic.jpovvol");
    std::vector<float> data = {0.0f, 0.0f};
    WriteVol(path, 1, 1, 1, 2, data, /*corrupt_magic=*/true);
    EXPECT_DEATH({ LoadFireVolumeFile(path); }, "");
    std::remove(path.c_str());
}

// 负向：数据被截断（少于 nx*ny*nz*channels*4 字节）→ 必须 crash。
TEST(FireVolume, TruncatedDataDies) {
    const std::string path = TempPath("trunc.jpovvol");
    std::vector<float> data = {0.0f, 0.0f};   // 只写 1 个体素，但头声明 2×2×1
    WriteVol(path, 2, 2, 1, 2, data);
    EXPECT_DEATH({ LoadFireVolumeFile(path); }, "");
    std::remove(path.c_str());
}

}  // namespace
}  // namespace jpov
