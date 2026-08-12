// ORM 通道解包 — glTF metallicRoughnessTexture → 3 张独立灰度 PNG
//
// glTF 的 metallicRoughnessTexture（即 Poly Haven "arm" 贴图）是
// ORM (Occlusion-Roughness-Metallic) 三通道打包：
//   R = Ambient Occlusion
//   G = Roughness
//   B = Metallic
//
// 本工具将一张 ORM 图片拆解为 3 张独立灰度 PNG，供 JPOV 的 PBRMaterial
// 管线使用（ao_tex / roughness_tex / metallic_tex 各绑定一张）。
//
// 使用方式：
//   1. 用 stb_image 读取 ORM 原图
//   2. 对每个通道调用 SaveGrayscaleToPng（或自行构造独立的 3 张图）
//   3. 返回的 PNG 内存块可写入 disk 用 TextureManager::LoadFromFile 加载，
//      或直接用 stb_image 再 decode（如果系统支持内存加载）
//
// 用法示例（见 gold test 文件）：
//   int ow, oh, oc;
//   unsigned char* orm = stbi_load("pliers_arm.jpg", &ow, &oh, &oc, 4);
//   std::string ao_png = SaveGrayscaleToPng(orm, ow, oh, 0);  // R channel
//   std::string rgh_png = SaveGrayscaleToPng(orm, ow, oh, 1); // G channel
//   std::string met_png = SaveGrayscaleToPng(orm, ow, oh, 2); // B channel
//   写入 disk → 用 TextureManager 注册 → 填入 PBRMaterial

#ifndef JPOV_ORM_UNPACK_H_
#define JPOV_ORM_UNPACK_H_

#include <string>
#include <vector>

namespace jpov {

// 从 RGBA 像素数据的单个通道提取为 PNG 字节流。
//
// 参数：
//   pixels: RGBA 像素数据（width * height * 4 字节）
//   width, height: 图像尺寸（像素 ≥ 1）
//   channel: 通道索引（0=R, 1=G, 2=B, 3=A）
//
// 返回：PNG 格式字节流（可直接写入 disk 用 TextureManager::LoadFromFile 加载）
// 失败时返回空 string。
//
// Pre-condition: pixels 非空，width > 0，height > 0
std::vector<unsigned char> ExtractChannelToPng(
    const unsigned char* pixels,
    int width,
    int height,
    int channel);

}  // namespace jpov

#endif  // JPOV_ORM_UNPACK_H_
