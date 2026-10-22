// 叶卡贴图的 CPU 处理（黑底→alpha、法线贴图、PNG 编解码）—— 查看器与 GLB 烘焙共用。
//
// 为什么抽出来：查看器「贴图预览」与烘焙器「写进 glb 的贴图」**必须是同一份像素**，
// 否则用户看到的叶子与导出的叶子不一致（这正是之前 gold 与 viewer 差 12% 的那类坑）。
// 这里只做 CPU 图像运算，不碰 GL。

#ifndef JPOV_TOOLS_VEGETATION_TOOL_V2_LEAF_TEXTURE_H_
#define JPOV_TOOLS_VEGETATION_TOOL_V2_LEAF_TEXTURE_H_

#include <cstdint>
#include <string>
#include <vector>

namespace jpov {
namespace vegetation {

// 叶卡 alpha 的裁切阈值。**必须只有一处**：覆盖率 mip 的二值化阈值与材质的
// `alpha_cutoff`（片元保留判据）必须相等，否则远景叶子会忽多忽少。
// 查看器（预览）与 GLB 烘焙（产物）都引用它。
constexpr float kLeafAlphaCutoff = 0.5f;

// 一张 RGBA8 叶卡贴图（行序 = 图片自上而下，与 PNG 文件一致）。
struct LeafTexture {
  int width = 0;
  int height = 0;
  std::vector<unsigned char> rgba;   // 4 字节/像素
};

// 读 PNG 并做「黑底 → alpha」：alpha = smoothstep((max(r,g,b) − lo)/(hi − lo))，
// lo = 0.5·black、hi = 1.5·black（与查看器 `--leaf-black` 的软过档一致）；
// 然后**反预乘**（观测色 = 叶色 × 覆盖率，不除回来边缘一圈黑）、
// 再把 RGB 向透明区扩散 `dilate_passes` 遍（防 mip/双线性混背景黑）。
//
// 失败返回 false 并写 *error（读不到文件 / 解码失败 / 全黑）。
bool LoadLeafTextureFromPng(const std::string& path, float black, int dilate_passes,
                            LeafTexture* out, std::string* error);

// 由 alpha + luma 生成切线空间法线贴图（RGBA8，alpha 通道恒 255）。
//
// height = 0.45·alpha + 0.55·luma → 两次 3-tap 盒模糊 → 中心差分 Sobel。
// 只依赖 CPU，确定性（同一输入必得同一输出，gold 友好）。
std::vector<unsigned char> BuildLeafNormalMap(const LeafTexture& tex, float depth);

// 只读 PNG 的像素尺寸（不解码像素）。
//
// 用途：树皮 UV 的 v 方向补偿 = 贴图宽/高（非方形贴图必须补偿，否则纹素不方正）。
// 查看器与烘焙各需要一次，只留这一处实现。
// 返回 false = 读不到文件 / 不是可识别的图片格式。
bool ReadPngSize(const std::string& path, int* width, int* height);

// RGBA8 → PNG 字节（内存内，不落盘）。
bool EncodePng(const std::vector<unsigned char>& rgba, int w, int h,
               std::vector<unsigned char>* out);

// 上下翻转行序（glTF 图片 v=0 在顶部，而我们的 UV 约定 v=0 在图片底边）。
void FlipRows(std::vector<unsigned char>* rgba, int w, int h);

}  // namespace vegetation
}  // namespace jpov

#endif  // JPOV_TOOLS_VEGETATION_TOOL_V2_LEAF_TEXTURE_H_
