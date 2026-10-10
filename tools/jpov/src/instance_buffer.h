// JPOV InstanceBuffer — 一次 Instanced draw 的 per-instance attribute 数据缓冲
//
// 生命周期归属（本文件存在的理由，2026-09-17 重构）：
//   per-instance 数据是「**这次 draw** 的输入」，由调用方每帧给出 —— 它**不属于任何 mesh**。
//   对照 GPUMesh：那是 RegisterMesh 时定死、之后不变的几何描述（positions/normals/…）。
//   把实例数据塞进 GPUMesh 有三个具体坏处（原实现踩过，已修）：
//     1) 语义污染：GPUMesh 不再是「不可变几何」的表示；
//     2) mesh 变成实例数据的**隐式单例载体** —— 同一 mesh 的两批实例、或两个 pass
//        （shadow / main）会写同一块 buffer，正确性只能靠「每个 draw 前紧邻一次 upload」
//        这个没人保护的隐式不变量；
//     3) 资源浪费：每个 mesh 都被无条件建实例 VBO，含永不 instancing 的静态几何。
//   因此实例数据由**渲染器持有**的可复用 buffer 承载，MeshManager 只管不变几何。
//
// GL 语义（为何是 Attach/Detach 而不是常驻）：
//   per-instance 属性必须挂在「本 draw 用的那个 VAO」上，而 GL 3.3 没有 DSA / 
//   glBindVertexBuffer，attribute→buffer 的绑定是 glVertexAttribPointer 时**写进 VAO** 的。
//   所以每次 draw 前用 AttachToVao 把本 buffer 挂到 mesh VAO 的 per-instance 槽上，
//   draw 后必须 DetachFromVao 摘掉 —— **enable/disable 严格配对**（symmetrical lifecycle）。
//   漏掉 Detach 会把 divisor=1 的启用态残留在 mesh VAO 上，泄漏给后续普通 draw。
//
// 用法（务必用 InstanceBufferBinding 保证配对，见文件末）：
//   model_buf.Upload(model_floats);          // 每实例 16 float（mat4）
//   pose_buf.Upload(pose_flat_ids);          // 每实例 4 float（pose/pose_lag 的平坦 texel 起点）
//   {
//     InstanceBufferBinding bind(mesh->vao, {&model_buf, &pose_buf});
//     glBindVertexArray(mesh->vao);
//     glDrawElementsInstanced(...);
//   }   // ← 出作用域自动 Detach

#ifndef JPOV_SRC_INSTANCE_BUFFER_H_
#define JPOV_SRC_INSTANCE_BUFFER_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace jpov {

// ==================== per-instance 属性布局 ====================

// 一个 InstanceBuffer 内**所有实例共享**的布局描述。
//   一个实例数据块被切成 slot_count 个连续 attribute slot，每个 slot 有
//   slot_components 个分量，实例间步长为 stride_floats 个 float。
// 例（GLSL mat4 在 GL 里没有原生 attribute，必须拆成 4 个 vec4 slot）：
//   mat4 → {base_loc, 4, 4, 16}
//   vec3 → {base_loc, 1, 3, 3}
struct InstanceAttrSpec {
    unsigned int base_loc = 0;      // 起始 attribute location
    int slot_count = 0;            // 每实例占几个连续 slot（mat4 = 4）
    int slot_components = 0;       // 每个 slot 的分量数（vec4 = 4，vec3 = 3）
    int stride_floats = 0;         // 每实例 float 数
    // integer_view：true 时用 glVertexAttribIPointer 以「原始整数位」解释（GLSL 里声明为
    //   uvec4），供位打包/半精度视图用；false（默认）走 glVertexAttribPointer + GL_FLOAT。
    //   两种视图下「每分量都是 4 字节」，故 stride_floats 的语义不变；只影响 GL 侧解释。
    bool integer_view = false;
};

// ---- 蒙皮带骨实例的布局（与本目录下 skinning_shader.h 的 layout(location=…) 逐字对应）----
//   本表是 2026-10-10 「per-instance attribute 布局重排」后的**定稿布局**（loc0..15 用满）：
//
//   顶点属性（每顶点，来自 GPUMesh，见 mesh_manager.h）：
//     0 aPos  1 aNormal  2 aTexCoord  3 aJoint  4 aWeight  5 aTangent
//     6 aRelax（vec2: lag_ratio, max_lag）★ 本次从实例首槽腾出，改作顶点属性
//   实例属性（divisor=1）：
//     7..10  aInstModel      mat4（4×vec4，列主序）                  stride 16 float
//     11     aInstPoseIds    4×float：pose.a/b + pose_lag.a/b（平坦 texel 起点）  stride 4 float
//     12     aInstMisc       uvec4（uint8 视图：2 ratio + 8 thick + 2 RGB888） stride 4 float(=16B)
//     13     aInstPartial    uvec4（8 half：pose 的 2 个 partial 四元数）   stride 4 float(=16B)
//     14     aInstPartialLag uvec4（8 half：lag  的 2 个 partial 四元数）   stride 4 float(=16B)
//     15     aInstPosLag     uvec4（8 half，用 3：lag 相对坐标 dx,dy,dz）  stride 4 float(=16B)
//
//   设计文档：tools/jpov/docs/jpov_instance_attr_lag_design.md §6。
//
// ⚠️ 这些常量是 host 侧与 shader 侧**唯一**的 layout 约定点：改 shader 的
//   layout(location=…) 必须同步改这里（反之亦然），否则静默读错槽位。
//   ⚠️ pose 用 float 而非 ivec2 + glVertexAttribIPointer：IPointer 按**原始整数位**
//   解释，把 float 上传后当整数读会得到天文数字（0x42B80000 = 1119354880）→ 取址出界。
inline constexpr InstanceAttrSpec kInstanceModelAttrSpec{/*base_loc*/ 7,  /*slot_count*/ 4,
                                                        /*slot_components*/ 4, /*stride*/ 16};
// pose 选择（含 LAG 的两个 flat id）：4×float，loc11。
//   .x/.y = pose.a / pose.b 在 atlas 里的平坦 texel 起点（= pose_idx * bone_count * 2）。
//   .z/.w = pose_lag.a / pose_lag.b 的平坦 texel 起点（LAG 未用时由 host 写成与 .x/.y 相同）。
//   ⚠️ 必须 float32：atlas 2048² 的 flat 上限 ~4.19M < 2²⁴，刚好够；压成 int16 会爆。
inline constexpr InstanceAttrSpec kInstancePoseIdsAttrSpec{/*base_loc*/ 11, /*slot_count*/ 1,
                                                           /*slot_components*/ 4, /*stride*/ 4};
// 杂项：uint8 视图（16 byte 严丝合缝），loc12。
//   byte0 = ratio(pose) /255；byte1 = ratio_lag /255；
//   byte2..9 = thick[0..7]，编码 0.01 + v/255 × 2.49 → [0.01,2.5]，步长 ~0.0097；
//   byte10..12 = color0(RGB888)；byte13..15 = color1(RGB888)。
//   位序/偏移的单一来源见 docs/jpov_instance_attr_lag_design.md §6.3。
inline constexpr InstanceAttrSpec kInstanceMiscAttrSpec{/*base_loc*/ 12, /*slot_count*/ 1,
                                                        /*slot_components*/ 4, /*stride*/ 4,
                                                        /*integer_view*/ true};
// 部位额外旋转（pose）：kNumPartialRotation(=2) 个**模型系**四元数（xyzw），
//   8 个 half 打包进 4 个 uint32（1 个 vec4 slot），loc13。与 skeleton_types.h 的
//   SkinnedInstanceState::partial_rotations 一一对应；shader 侧手工 HalfToFloat 解包
//   （GLSL 330 无 unpackHalf2x16，见 skinning_shader.h）。
inline constexpr InstanceAttrSpec kInstancePartialAttrSpec{/*base_loc*/ 13, /*slot_count*/ 1,
                                                           /*slot_components*/ 4, /*stride*/ 4,
                                                           /*integer_view*/ true};
// 部位额外旋转（lag）：与 loc13 同构，存 pose_lag 的 2 个模型系四元数，loc14。
inline constexpr InstanceAttrSpec kInstancePartialLagAttrSpec{/*base_loc*/ 14, /*slot_count*/ 1,
                                                              /*slot_components*/ 4, /*stride*/ 4,
                                                              /*integer_view*/ true};
// lag 相对坐标：前 3 个 half = (dx, dy, dz)（模型/锚点系的小值域偏移），loc15。
//   见设计 §6.6；half 需小值域 + 避免相减抵消。
inline constexpr InstanceAttrSpec kInstancePosLagAttrSpec{/*base_loc*/ 15, /*slot_count*/ 1,
                                                          /*slot_components*/ 4, /*stride*/ 4,
                                                          /*integer_view*/ true};

// ==================== 位打包 codec（host 侧；与上方布局表**同源**） ====================
//   设计 §7.3「编解码单一来源」：打包/解包逻辑与偏移常量放在**同一头文件**（本文件=布局表所在处），
//   shader 侧的偏移注释指向本文件；改布局时 host 与 GLSL 一起改。
//   下方 Encode/Decode 与 GLSL（skinning_shader.h 的 MiscRatio / ThicknessOfChannel /
//   UnpackHalf2）**逐位对应**，且由 instance_buffer_test 的 codec 往返段做验证（把位打包从
//   “天才代码”降级为普通代码的关键）。

// --- loc12（aInstMisc）uint8 视图的 byte 偏移（单一来源）---
inline constexpr int kMiscByteRatio    = 0;   // ratio(pose)   /255
inline constexpr int kMiscByteRatioLag = 1;   // ratio_lag     /255
inline constexpr int kMiscByteThickBase = 2;  // thick[0..7]   占 byte 2..9
inline constexpr int kMiscByteColor0   = 10;  // color0 RGB888 占 byte 10..12
inline constexpr int kMiscByteColor1   = 13;  // color1 RGB888 占 byte 13..15

// thick 量化区间（[0.01, 2.5]，256 级，绝对步长 ~0.0097）。
//   ⚠️ 1.0 不能精确表示（最近值 101/255 → 0.9960）；默认全 1.0 时由 host 开关
//   uThicknessEnabled=0 **整段跳过** ⇒ 逐字节零回归，不靠“乘单位元”。
inline constexpr float kThickMin = 0.01f;
inline constexpr float kThickMax = 2.5f;

// thick float → uint8（越界 clamp；步长 ~0.0097）。
inline uint8_t EncodeThickness(float mu) {
    float t = (mu - kThickMin) / (kThickMax - kThickMin);
    t = (t < 0.0f) ? 0.0f : ((t > 1.0f) ? 1.0f : t);
    return static_cast<uint8_t>(t * 255.0f + 0.5f);
}

// thick uint8 → float（与 shader 的 ThicknessOfChannel 逐位一致）。
inline float DecodeThickness(uint8_t v) {
    return kThickMin + static_cast<float>(v) / 255.0f * (kThickMax - kThickMin);
}

// float ↔ IEEE 754 binary16（便携，不依赖 _Float16；兼容 MinGW）。
//   子正规（|f| < 2^-14）一律 flush 到 0（误差 < 6e-5，对单位四元数/小偏移可接受）。
//   shader 侧的 HalfToFloat（skinning_shader.h）采用同一约定。
//   截断而非舍入（误差 ≤ 1 ulp），不引入进位扰动。
inline uint16_t FloatToHalfBits(float f) {
    uint32_t x = 0;
    std::memcpy(&x, &f, sizeof(x));
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int32_t exp = static_cast<int32_t>((x >> 23) & 0xFFu) - 127 + 15;
    const uint32_t mant = x & 0x7FFFFFu;
    if (exp <= 0) {
        return static_cast<uint16_t>(sign);  // 零 / 子正规 → 0
    }
    if (exp >= 31) {
        return static_cast<uint16_t>(sign | 0x7C00u);  // Inf（不期望出现）
    }
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) | (mant >> 13));
}

// IEEE 754 binary16 → float（与 shader 侧 HalfToFloat 逐位一致）。
inline float HalfBitsToFloat(uint16_t h) {
    const uint32_t sign = (static_cast<uint32_t>(h) & 0x8000u) << 16;
    const uint32_t exp = (static_cast<uint32_t>(h) >> 10) & 0x1Fu;
    const uint32_t mant = static_cast<uint32_t>(h) & 0x3FFu;
    if (exp == 0) {
        float z = 0.0f;
        std::memcpy(&z, &sign, sizeof(z));  // ±0
        return z;
    }
    uint32_t bits;
    if (exp == 31) {
        bits = sign | 0x7F800000u | (mant << 13);  // Inf / NaN
    } else {
        bits = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

// 把 8 个 half 分量（两两成对）打包进 4 个 uint32（低 16 位 = 首分量）。
//   与 shader 侧 UnpackHalf2 的 .x=低 16 位 / .y=高 16 位 对应。
inline void PackHalf8(const float src[8], uint32_t out[4]) {
    for (int k = 0; k < 4; ++k) {
        const uint16_t lo = FloatToHalfBits(src[k * 2 + 0]);
        const uint16_t hi = FloatToHalfBits(src[k * 2 + 1]);
        out[k] = static_cast<uint32_t>(lo) | (static_cast<uint32_t>(hi) << 16);
    }
}

// ==================== InstanceBuffer ====================

// 承载一批实例的 per-instance attribute 数据。
// 构造时给布局；Upload 可反复调用（同一缓冲复用，容量不够才扩容）。
class InstanceBuffer {
public:
    InstanceBuffer() = default;
    ~InstanceBuffer();

    InstanceBuffer(const InstanceBuffer&) = delete;
    InstanceBuffer& operator=(const InstanceBuffer&) = delete;

    explicit InstanceBuffer(const InstanceAttrSpec& spec) : spec_(spec) {}

    // 上传一批实例数据（全部实例的本实例字段，按实例连续排列）。
    // data 按 float 数组组织（每分量 4 字节）；对 integer_view 的槽，host 侧把打包好的
    //   uint32 按位重解释为 float 存入（字节完全一致），GL 侧用 glVertexAttribIPointer
    //   以原始整数位读出（见 AttachToVao）。
    // Pre-condition: data.size() 是 spec_.stride_floats 的整数倍且 > 0。
    void Upload(const std::vector<float>& data);

    // 把本缓冲挂到 vao 的 per-instance 槽（glVertexAttribPointer + enable + divisor=1）。
    // ⚠️ 配对使用：挂上后必须 DetachFromVao（推荐用 InstanceBufferBinding 自动管）。
    void AttachToVao(unsigned int vao) const;

    // 从 vao 摘除 per-instance 槽（disable + divisor 归 0），不留残留启用态。
    void DetachFromVao(unsigned int vao) const;

    const InstanceAttrSpec& spec() const { return spec_; }
    int instance_count() const { return instance_count_; }
    unsigned int vbo() const { return vbo_; }

private:
    // 惰性建 GL buffer（GL context 可能在渲染器构造之后才就绪）。
    void EnsureBuffer();

    InstanceAttrSpec spec_{};
    unsigned int vbo_ = 0;
    size_t capacity_bytes_ = 0;
    int instance_count_ = 0;
};

// ==================== InstanceBufferBinding（RAII 配对守卫） ====================

// 构造时把若干 InstanceBuffer 挂到 vao 上，析构时自动摘除。
// 用途：**结构性地**保证 enable/disable 严格配对（symmetrical lifecycle）——
//   即使将来有人加 pass、改 draw 路径，也不可能「挂上忘了摘」，
//   从而不可能把 divisor=1 的启用态泄漏给后续普通 draw。
//
//   用法：
//     {
//       InstanceBufferBinding bind(mesh->vao, {&model_buf, &pose_buf});
//       glBindVertexArray(mesh->vao);
//       glDrawElementsInstanced(...);
//     }
class InstanceBufferBinding {
public:
    InstanceBufferBinding(unsigned int vao,
                          std::initializer_list<const InstanceBuffer*> buffers);
    ~InstanceBufferBinding();

    InstanceBufferBinding(const InstanceBufferBinding&) = delete;
    InstanceBufferBinding& operator=(const InstanceBufferBinding&) = delete;

private:
    unsigned int vao_ = 0;
    std::vector<const InstanceBuffer*> buffers_;
};

}  // namespace jpov

#endif  // JPOV_SRC_INSTANCE_BUFFER_H_
