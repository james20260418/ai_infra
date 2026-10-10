// JPOV InstancedObjectRenderer — 静态实例渲染器的 per-instance 属性布局 spec（独立系统）
//
// 本文件是什么：
//   InstancedObjectRenderer（静态模型批量实例渲染）**自己拥有**的一份 per-instance
//   属性布局约定 —— 用哪些 attribute location、每个 location 怎么切、留哪些空槽给
//   后续字段（如植被的颜色 / 摆动相位）。它是「静态实例路径」的单一来源：绘制端据此
//   构造 InstanceBuffer，测试据此守住布局自洽。
//
// 为什么单开一套（2026-10-10）：
//   静态实例路径早期**直接复用**蒙皮路径的实例缓冲与 spec（src/instance_buffer.h 的
//   kInstance*AttrSpec）。两条路径共用 0..15 的 location 预算，任何一方重排都会把另一方
//   “静默拖下水”：PR #169 的 LAG 布局重排就把静态实例的模型槽从 loc6..9 挤到 loc7..10，
//   得另开一个 commit 才能让实例 shader 对齐。为让静态实例路径能**独立演进**，这里把它
//   自己的布局抽成独立 spec 系统，与蒙皮布局互不引用。
//
// attribute location 预算（0..15；GL 3.3 至少保证 16 个）：
//   每顶点（静态路径用到的）—— 0 aPos  1 aNormal  2 aTexCoord  5 aTangent
//   每顶点（蒙皮路径专属，静态路径不用）—— 3 aJoint  4 aWeight  6 aRelax
//   每实例（静态路径，本文件定义）—— 7..10 aInstModel（mat4，列主序，4×vec4）
//   空闲可预留（静态路径未来可用的槽）—— 11 12 13 14 15（蒙皮专属 3/4/6 亦空闲）
//   ⚠️ location 编号只在「同一 draw 的同一 program」内有意义。静态与蒙皮是互斥的
//      program，可各自占用相同的编号而不冲突（二者模型矩阵都落在 7..10 即为例）。
//      本文件只保证「静态路径自身」不越界、不重叠。
//
// 与蒙皮布局的关系：蒙皮布局见 src/instance_buffer.h（kInstance*AttrSpec，loc7..15）。
//   两份 spec 由各自模块拥有、各自演进，共用同一段编号空间但**互不引用**。
//
// 新增一个 per-instance 字段的步骤（维护手册）：
//   1) 在此登记 spec（挑一个空闲槽）、更新上面的预算表、把它加进 kActiveAttrSpecs；
//   2) 三个实例 VS（kMeshVs3dPBRInstanced / kMeshVs3dPBRFullInstanced / kShadowVsInstanced）
//      加对应 `layout(location = N) in ...`，并同步本文件的 base_loc；
//   3) Renderer 加一个 InstanceBuffer（用本文件的 spec），绘制端挂进 InstanceBufferBinding。

#ifndef JPOV_INSTANCED_OBJECT_ATTRS_H_
#define JPOV_INSTANCED_OBJECT_ATTRS_H_

#include "tools/jpov/src/instance_buffer.h"

namespace jpov {
namespace instanced_object {

// ---- 静态路径用到的**顶点**属性槽（来自 GPUMesh，非 per-instance）----
//   仅用于自检：per-instance spec 不得与这些槽重叠。
inline constexpr unsigned int kVertexAttrLocPos      = 0;
inline constexpr unsigned int kVertexAttrLocNormal   = 1;
inline constexpr unsigned int kVertexAttrLocTexCoord = 2;  // 仅含 UV 的 mesh
inline constexpr unsigned int kVertexAttrLocTangent  = 5;  // 仅含 tangent 的 mesh

// ---- per-instance 属性 spec ----

// 每实例摆放矩阵（mat4，列主序）：loc7..10 拆 4 个 vec4 slot，每实例 16 float。
//   与 instanced_object_renderer.h 三个实例 VS 的
//   `layout(location = 7..10) in vec4 aInstCol0..3` **逐字对应**（三者一致）。
inline constexpr InstanceAttrSpec kModelAttrSpec{/*base_loc*/ 7, /*slot_count*/ 4,
                                                 /*slot_components*/ 4, /*stride*/ 16};

// 本路径当前启用的 per-instance spec 清单（绘制端据此挂 buffer；测试据此做全量自检）。
//   新增字段时把新 spec 追加进来。
inline constexpr InstanceAttrSpec kActiveAttrSpecs[] = {kModelAttrSpec};

// ---- 布局自检谓词（编译期 / 运行期共用；供 static_assert 与测试）----

// spec 合法：槽数 / 分量为正、区间不越 16、stride 与槽位一致。
constexpr bool SpecIsValid(const InstanceAttrSpec& s) {
    return s.slot_count > 0 && s.slot_components > 0 &&
           s.base_loc + static_cast<unsigned int>(s.slot_count) <= 16u &&
           s.slot_count * s.slot_components == s.stride_floats;
}

// 两个 spec 的槽区间不相交。
constexpr bool SpecsDisjoint(const InstanceAttrSpec& a, const InstanceAttrSpec& b) {
    const unsigned int a_end = a.base_loc + static_cast<unsigned int>(a.slot_count);
    const unsigned int b_end = b.base_loc + static_cast<unsigned int>(b.slot_count);
    return a_end <= b.base_loc || b_end <= a.base_loc;
}

// loc 是否为静态路径的顶点属性槽。
constexpr bool IsVertexAttrLoc(unsigned int loc) {
    return loc == kVertexAttrLocPos || loc == kVertexAttrLocNormal ||
           loc == kVertexAttrLocTexCoord || loc == kVertexAttrLocTangent;
}

// spec 的任一槽都不落在顶点属性槽上。
constexpr bool SpecAvoidsVertexAttrs(const InstanceAttrSpec& s) {
    for (int i = 0; i < s.slot_count; ++i) {
        if (IsVertexAttrLoc(s.base_loc + static_cast<unsigned int>(i))) {
            return false;
        }
    }
    return true;
}

// ---- 编译期门禁（越界 / 重叠 / stride 不符即编译失败）----
static_assert(SpecIsValid(kModelAttrSpec), "静态实例模型 spec 非法：越 16 或 stride 不符");
static_assert(SpecAvoidsVertexAttrs(kModelAttrSpec), "静态实例模型 spec 与顶点属性槽重叠");
// 新增 spec 时在此追加：static_assert(SpecsDisjoint(kModelAttrSpec, kXxxSpec), "...");

}  // namespace instanced_object
}  // namespace jpov

#endif  // JPOV_INSTANCED_OBJECT_ATTRS_H_
