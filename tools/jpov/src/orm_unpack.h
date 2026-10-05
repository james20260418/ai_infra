// ORM 通道解包 — glTF metallicRoughnessTexture 辅助
//
// glTF 的 metallicRoughnessTexture（即 Poly Haven "arm" 贴图）是
// ORM (Occlusion-Roughness-Metallic) 三通道打包：
//   R = Ambient Occlusion
//   G = Roughness
//   B = Metallic
//
// 本模块提供**像素级**工具：
//   - RgbaToPng: 把任意通道数的像素编码为 PNG 字节流（用于内嵌图回退重编）。
//   - ORM 的 G/B 拆包现由 renderer 直接以像素上传（TextureManager::FromPixels），
//     不再在本模块产出 PNG。

#ifndef JPOV_ORM_UNPACK_H_
#define JPOV_ORM_UNPACK_H_

#include <string>
#include <vector>

namespace jpov {

// 把【任意通道数】的像素数据编码为 PNG 字节流（整图，非单通道）。
//
// 用于把解码后的图像数据重新编码为 PNG（如内嵌 bufferView 贴图在无法取到原始
// 压缩字节时，由 stb 解码后的 RGBA 像素重编兜底）。
//
// 参数：
//   pixels: 像素数据（width * height * components 字节）
//   width, height: 尺寸（≥1）
//   comps: 每像素通道数（1..4；超出按 4 处理）
//
// 返回：PNG 字节流；失败返回空 vector。
//
// Pre-condition: pixels 非空，width > 0，height > 0，comps 1..4
std::vector<unsigned char> RgbaToPng(
    const unsigned char* pixels,
    int width,
    int height,
    int comps);

}  // namespace jpov

#endif  // JPOV_ORM_UNPACK_H_
