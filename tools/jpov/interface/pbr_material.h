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

// 顶点 base tint 分区的用户可选颜色数（**MVP = 2**）。
//
// 语义：`MeshData::base_tint` 逐顶点存一个 uint8 索引，取值 0..kNumBaseTintColors：
//   0               = 该顶点不参与换色（保持 baseColor 贴图原色）
//   1..kNum...      = 用 `BaseTint::color[i-1]` 乘 baseColor
// 着色器按 **flat（provoking vertex）** 读索引 ⇒ 一个三角形整体归属一个色。
//
// 为什么是 2：MVP 只开放两个调色板通道（对应 2×RGB 滑条 + 2 个 flat varying）。
// 顶点属性本身是完整 uint8（0..255 的值空间）。扩通道数时本常量、`BaseTint::color[]`、
// object3d FS 的 uniform/分支三处要一起改；**顶点布局不用动**（id 仍是 loc6.z 的一个 float）。
inline constexpr int kNumBaseTintColors = 2;

// base tint 调色板（**运行期参数，不是资产数据**）。
//
// enabled == false ⇒ 片元着色器**完全不做这次乘法**（开关保护，A/B 用）。
// enabled == true 且颜色全白 ⇒ 乘法恒等，观感与不开完全一致。
//
// 默认 `enabled = false`、颜色全白：引擎默认零额外成本、也不改变任何既有画面的语义；
// 需要该功能的工具（model viewer / clothing tool）显式打开，并把默认色设成红/绿
// （加载/刷完立刻能看出两个分区，同时两个通道都可随时改）。
struct BaseTint {
    bool enabled = false;
    // 索引 i 对应顶点 tint id = i + 1。默认白 = 乘性恒等。
    Color color[kNumBaseTintColors] = {{1.0f, 1.0f, 1.0f, 1.0f},
                                       {1.0f, 1.0f, 1.0f, 1.0f}};
};

// 材质透明模式。
//   kOpaque：不透明（默认）。alpha 通道被忽略。
//   kMask  ：alpha test（cutout）——采样 baseColor 贴图的 alpha，低于 alpha_cutoff
//            的片元直接 discard。不排序、写深度，与阴影 / picking 兼容。
//   kBlend ：半透明混合（无折射「塑料片 / 玻璃片」）——片元**不进主 3D FBO**，
//            改由独立的透明 pass 以 **加权混合 OIT（Weighted Blended OIT）** 渲进
//            离屏 accum/reveal 缓冲，再由体积雾合成 pass 与雾一起按深度积分合成
//            （见 docs/jpov_transparent_glass_design.md）。语义：
//              · alpha = base_color.a（无贴图）或 base_color.a * 贴图 alpha（有色贴图）；
//              · **不写主 FBO 颜色**、不写场景深度、不投阴影（玻璃不产生不透明影）；
//              · 半透明不参与 picking / highlight（本版）。
//            与 kMask 一样**另起 program 变体**（宏 JPOV_ALPHA_BLEND），使不透明 program
//            不含 blend 分支（不降级实心物体的 early-Z 深度快路径）。
enum class AlphaMode : uint8_t { kOpaque, kMask, kBlend };

struct PBRMaterial {
    // 便利构造：纯色材质（无纹理/法线/emissive/AO）。
    // metallic / roughness 给一组居中默认值（塑料感）：
    //   metallic = 0.0（非金属）、roughness = 0.5（半光滑）。
    static PBRMaterial SolidColor(const Color& c) {
        PBRMaterial m;
        m.base_color = c;
        m.metallic = 0.0f;
        m.roughness = 0.5f;
        return m;
    }

    // 便利构造：纯色材质 + 显式 metalic/roughness（其余同 SolidColor）。
    // 调用方想自定义金属度/粗糙度时用此接口。
    static PBRMaterial SolidColorMR(const Color& c, float metallic,
                                    float roughness) {
        PBRMaterial m = SolidColor(c);
        m.metallic = metallic;
        m.roughness = roughness;
        return m;
    }

    // baseColor: 常值 fallback（base_color_tex ≠ 0 时走纹理采样）
    Color base_color;
    uint32_t base_color_tex = 0;

    // 顶点 base tint 调色板（见上方 BaseTint 注释）。
    //   着色：base_color_final = baseColor_texel.rgb × base_tint.color[id-1]
    //   ⚠️ 调色板只是「颜色查表」：**mesh 必须带 kBaseTint 的逐顶点 id 才会生效**；
    //      没有 id 的 mesh 即使设了 enabled/颜色也不会有任何变化（这是设计，不是 bug）。
    //   适用路径：**无 per-instance 颜色通道**的 Object3D / GltfObject 绘制（本 MVP）。
    //   蒙皮 / 实例路径暂未接入（见 tools/jpov/docs/jpov_base_tint_design.md）。
    BaseTint base_tint;

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

    // 透明模式：kOpaque（默认，忽略 alpha）/ kMask（cutout alpha test，见 AlphaMode）。
    AlphaMode alpha_mode = AlphaMode::kOpaque;
    // alpha test 阈值（仅 alpha_mode==kMask 生效）：baseColor.a < alpha_cutoff 的片元
    // 丢弃。Pre-condition（kMask 时）：base_color_tex 非 0（无贴图则无 alpha 可测）。
    float alpha_cutoff = 0.5f;

    // 双面渲染：true 时不剔背面（配合 FS 的 gl_FrontFacing 法线翻转）。
    // 来源：glTF doubleSided（缺省 false）；薄片 / 单面几何（叶子卡片、薄纱）需要它。
    bool double_sided = false;
};

}  // namespace jpov

#endif  // JPOV_INTERFACE_PBR_MATERIAL_H_
