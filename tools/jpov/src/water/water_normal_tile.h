// JPOV 水皮 —— **波浪法线 flipbook 纹理**（CPU 生成 → GPU 纹理数组 + mip 链）
//
// 为什么要有这个东西（Danis 2026-10-28 提的问题）：
//   "法线纹理也是纹理，就是带 flipbook 而已，每帧都 mip，直接采样更直接自然？"
//   —— 对，完全可以，而且**不需要 FFT**。FFT 只是"生成波浪"的一种方式（谱可控、能量守恒），
//   跟"过滤"是两件事；过滤用 GPU 的 mip/各向异性就够了。
//
// 本文件负责把程序化波浪**烘成可平铺的法线（斜率）图集**：
//   * 空间可平铺：每个八度用**整数波数**（nx, nz），所以 tile 边界天然无缝；
//   * 时间可循环：每帧的相位推进是**整数圈**（cycles 圈 / frames 帧）⇒ 第 frames 帧 == 第 0 帧；
//   * 存**斜率**（-∇h）而不是归一化法线：斜率是线性量，硬件 mip 平均它才有物理意义
//     （直接平均归一化法线是错的 —— 见 LEAN/LEADR 那套）；
//   * mip 链交给 glGenerateMipmap，采样用 LINEAR_MIPMAP_LINEAR + 各向异性。
//
// 已知简化（写清楚，别当 bug）：
//   * B/A 通道暂时只填 1.0 —— 真正的 LEADR 要在 B/A 里存**斜率方差**，让远处高光变宽；
//     本版只做"纹理 + mip"，方差留给解析路径（water_skin_shader.h 的 mode 3）。
//   * 单张 tile 会重复：远处能看出周期性。工程上要叠 2~3 个不同尺度/方向的 tile。

#ifndef JPOV_SRC_WATER_WATER_NORMAL_TILE_H_
#define JPOV_SRC_WATER_WATER_NORMAL_TILE_H_

namespace jpov {

// 一张烘好的波浪法线图集（GL_TEXTURE_2D_ARRAY：每层一帧，每层自带 mip 链）。
struct WaterNormalTile {
    unsigned int tex = 0;    // 0 = 没建
    int size = 0;            // 每层边长（texel）
    int frames = 0;          // 层数（flipbook 帧数）
};

// 生成并上传。参数都带单位：
//   size      : 每层边长（建议 128~512）
//   frames    : flipbook 帧数（4~16）
//   tile_m    : 一张 tile 覆盖多少米（决定最细波长；建议 8~32 m）
// Pre-condition: 已创建 GL context；size > 0、frames > 0、tile_m > 0。
// 返回的纹理：RGBA8、REPEAT、三线性 + （有扩展时）各向异性 8×。
// 释放用 ReleaseWaterNormalTile()。
WaterNormalTile BuildWaterNormalTile(int size, int frames, float tile_m);

// 释放（幂等）。
void ReleaseWaterNormalTile(WaterNormalTile* tile);

}  // namespace jpov

#endif  // JPOV_SRC_WATER_WATER_NORMAL_TILE_H_
