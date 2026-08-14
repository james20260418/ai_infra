// JPOV PBR 材质参数 —— 世界空间 3D 物体着色用
//
// 描述一个 3D 物体的 PBR 材质。每个通道要么用常值（fallback），
// 要么用纹理（逐像素材质参数场，采样后进 BRDF）。通道与对应是否有纹理
// 的关系由各字段表达：
//   - base_color / emissive / ao 为颜色，分别配套 *_tex（0 = 无纹理）
//   - metallic / roughness 为标量，分别配套 has_*_tex + *_tex
//   - normal_scale 缩放法线贴图扰动强度
//
// 约定：纹理句柄 0 表示无纹理，此时使用对应的常值 fallback。
//       *_tex 非 0 时采样该纹理作为逐像素材质参数，忽略对应常值。
//
// 本类型是叶子类型，被 render_command.h（Object3DCommand 字段）和
// gltf_object.h（GltfPrimitive 字段）共同引用，故独立成头文件，
// 避免「render_command ↔ gltf_object」的 include 循环。
//
// Color 及其常量也从 render_command.h 迁至此，供各命令结构体共用。

#ifndef JPOV_INTERFACE_PBR_MATERIAL_H_
#define JPOV_INTERFACE_PBR_MATERIAL_H_

#include <cstdint>

namespace jpov {

// RGBA 颜色，分量 [0, 1]
struct Color {
    float r, g, b, a;
};

// 常用颜色常量
extern const Color kColorRed;
extern const Color kColorGreen;
extern const Color kColorBlue;
extern const Color kColorWhite;
extern const Color kColorBlack;
extern const Color kColorTransparent;

struct PBRMaterial {
    // 便利构造：纯色材质（metallic=0, roughness=1，无纹理/法线/emissive/AO）
    static PBRMaterial SolidColor(const Color& c) {
        PBRMaterial m;
        m.base_color = c;
        return m;
    }

    // baseColor: 常值 fallback（base_color_tex ≠ 0 时走纹理采样）
    Color base_color;
    uint32_t base_color_tex = 0;

    // metallic / roughness: scalar or texture
    float metallic = 0.0f;
    bool has_metallic_tex = false;
    uint32_t metallic_tex = 0;
    float roughness = 1.0f;
    bool has_roughness_tex = false;
    uint32_t roughness_tex = 0;

    // 法线贴图（扰动法线，采样的 TBN 变换）
    float normal_scale = 1.0f;
    uint32_t normal_tex = 0;     // 法线贴图

    // emissive: color or texture
    Color emissive{0.0f, 0.0f, 0.0f, 1.0f};   // 默认无自发光
    uint32_t emissive_tex = 0;

    // AO（环境光遮蔽）: color or texture
    // 常值取 .r 作为标量强度（灰度）；默认 1.0 = 无遮蔽。
    Color ao{1.0f, 1.0f, 1.0f, 1.0f};
    uint32_t ao_tex = 0;
};

}  // namespace jpov

#endif  // JPOV_INTERFACE_PBR_MATERIAL_H_
