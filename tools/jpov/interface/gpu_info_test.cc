// JPOV GpuInfo 分类单元测试（纯 CPU，不碰 GL）
//
// 为什么值得单独测：`kind` 是唯一会被用来做**门禁**的字段（"拒绝在软件光栅上跑"），
// 判错方向的代价不对称——把真硬件误判成软件会误伤用户，把软件误判成硬件则让慢到
// 不可用的运行悄悄通过。所以这里把「哪些字符串必须命中」逐条钉死。
//
// 断言清单：
//   1. 空串 → kUnknown（查询失败，不是"硬件"）
//   2. Mesa 三种 CPU 光栅器名字 → kCpuRaster（含大小写混合、带版本尾巴的真实串）
//   3. Windows 无驱动时的 "GDI Generic" → kCpuRaster
//   4. 真实 GPU 串 → kHardware（NVIDIA / AMD / Intel / Mesa 硬件驱动）
//   5. 子串匹配是**包含**而不是相等（真实串都带尾巴，如 "/PCIe/SSE2"）
//   6. GpuKindName 三态名字稳定（HUD / 日志按它打印）

#include <string>

#include <gtest/gtest.h>

#include "tools/jpov/interface/gpu_info.h"

namespace {

TEST(ClassifyGpuRenderer, EmptyIsUnknown) {
    EXPECT_EQ(jpov::ClassifyGpuRenderer(""), jpov::GpuKind::kUnknown);
}

TEST(ClassifyGpuRenderer, MesaCpuRasterizersAreDetected) {
    // 真实串（本仓库 CI 上就是第一行这种）。
    EXPECT_EQ(jpov::ClassifyGpuRenderer("llvmpipe (LLVM 20.1.2, 256 bits)"),
              jpov::GpuKind::kCpuRaster);
    EXPECT_EQ(jpov::ClassifyGpuRenderer("llvmpipe"), jpov::GpuKind::kCpuRaster);
    EXPECT_EQ(jpov::ClassifyGpuRenderer("softpipe"), jpov::GpuKind::kCpuRaster);
    EXPECT_EQ(jpov::ClassifyGpuRenderer("Mesa swrast"), jpov::GpuKind::kCpuRaster);
}

TEST(ClassifyGpuRenderer, MatchIsCaseInsensitive) {
    EXPECT_EQ(jpov::ClassifyGpuRenderer("LLVMpipe"), jpov::GpuKind::kCpuRaster);
    EXPECT_EQ(jpov::ClassifyGpuRenderer("SoftPipe (Mesa)"),
              jpov::GpuKind::kCpuRaster);
}

TEST(ClassifyGpuRenderer, WindowsSoftwareFallbacksAreDetected) {
    EXPECT_EQ(jpov::ClassifyGpuRenderer("GDI Generic"), jpov::GpuKind::kCpuRaster);
    EXPECT_EQ(jpov::ClassifyGpuRenderer("Microsoft Basic Render Driver"),
              jpov::GpuKind::kCpuRaster);
    EXPECT_EQ(jpov::ClassifyGpuRenderer("Apple Software Renderer"),
              jpov::GpuKind::kCpuRaster);
}

TEST(ClassifyGpuRenderer, RealGpusAreHardware) {
    // 关键的反向断言：命中软件黑名单**不能**误伤真硬件。
    EXPECT_EQ(jpov::ClassifyGpuRenderer("NVIDIA GeForce RTX 4060/PCIe/SSE2"),
              jpov::GpuKind::kHardware);
    EXPECT_EQ(jpov::ClassifyGpuRenderer("AMD Radeon RX 6600 (radeonsi, navi23, LLVM 15.0.7)"),
              jpov::GpuKind::kHardware);
    EXPECT_EQ(jpov::ClassifyGpuRenderer("Mesa Intel(R) UHD Graphics 620 (CML GT2)"),
              jpov::GpuKind::kHardware);
    EXPECT_EQ(jpov::ClassifyGpuRenderer("Mesa D3D12 (NVIDIA GeForce RTX 3060)"),
              jpov::GpuKind::kHardware);
}

TEST(GpuKindName, StableNames) {
    EXPECT_STREQ(jpov::GpuKindName(jpov::GpuKind::kUnknown), "unknown");
    EXPECT_STREQ(jpov::GpuKindName(jpov::GpuKind::kHardware), "hardware");
    EXPECT_STREQ(jpov::GpuKindName(jpov::GpuKind::kCpuRaster), "cpu-raster");
}

}  // namespace
