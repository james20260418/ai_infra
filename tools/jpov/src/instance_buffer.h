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
//   model_buf.Upload(model_floats);          // 每实例 16 float
//   pose_buf.Upload(pose_floats);            // 每实例 3 float（蒙皮用；纯静态实例不需要）
//   {
//     InstanceBufferBinding bind(mesh->vao, {&model_buf, &pose_buf});
//     glBindVertexArray(mesh->vao);
//     glDrawElementsInstanced(...);
//   }   // ← 出作用域自动 Detach

#ifndef JPOV_SRC_INSTANCE_BUFFER_H_
#define JPOV_SRC_INSTANCE_BUFFER_H_

#include <cstddef>
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
};

// ---- 蒙皮带骨实例的布局（与 skinning_shader.h 的 layout(location=…) 逐字对应）----
//   摆放矩阵 model：loc6..9（mat4 拆 4 个 vec4 slot），每实例 16 float
//   pose 选择     ：loc10   （vec3: pose_col_a, pose_col_b, ratio），每实例 3 float
//   部位粗细系数  ：loc11..12（8 个 float = 2 个 vec4），每实例 8 float
// ⚠️ 这三个常量是 host 侧与 shader 侧**唯一**的 layout 约定点：改 shader 的
//   layout(location=…) 必须同步改这里（反之亦然），否则静默读错槽位。
//   ⚠️ pose 用 float 而非 ivec2 + glVertexAttribIPointer：IPointer 按**原始整数位**
//   解释，把 float 上传后当整数读会得到天文数字（0x42B80000 = 1119354880）→ 取址出界。
inline constexpr InstanceAttrSpec kInstanceModelAttrSpec{/*base_loc*/ 6,  /*slot_count*/ 4,
                                                        /*slot_components*/ 4, /*stride*/ 16};
inline constexpr InstanceAttrSpec kInstancePoseAttrSpec{/*base_loc*/ 10, /*slot_count*/ 1,
                                                        /*slot_components*/ 3, /*stride*/ 3};
// 部位粗细：每实例 kNumThicknessGroup(=8) 个组系数，**两**个 vec4 slot（loc11/12）。
//   为什么拆两个 vec4 而不做成 vec8：GL 单属性最大 vec4；shader 侧再按组号（0..7）取。
//   与 skeleton_types.h 的 kNumThicknessGroup / SkinnedInstanceState::thickness_scales 一一对应
//   —— 组数的**唯一定义处**是 kNumThicknessGroup（skeleton_types.h）；本常量与 shader 的
//   `layout(location = 11/12)` 声明必须同步（两个 vec4 ⇒ stride 8 float）。
inline constexpr InstanceAttrSpec kInstanceThicknessAttrSpec{/*base_loc*/ 11, /*slot_count*/ 2,
                                                             /*slot_components*/ 4, /*stride*/ 8};

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
