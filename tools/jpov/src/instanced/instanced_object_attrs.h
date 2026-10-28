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
//                              6 aFlex（植被风 (joint_id, phase)；仅 kWind mesh）
//   每顶点（蒙皮路径专属，静态路径不用）—— 3 aJoint  4 aWeight  6 aRelax
//   每实例（静态路径，本文件定义）—— 7..10 aInstModel（mat4，列主序，4×vec4）
//                                    11     aInstWindDirBlow（vec4：风向.xyz + 吹倒比例）
//                                    12     aInstTint0（vec3：逐实例 color0）
//                                    13     aInstTint1（vec3：逐实例 color1）
//                                    14     aInstTintEnabled（float：0/1 开关）
//                                    15     aInstWindParams（vec4：相位 / 摆幅 / 频率 / 阵风相位）
//   空闲可预留（静态路径未来可用的槽）—— 3 4
//     · loc3：2026-10-26 前被植被风的两关节权重 (w0,w1) 占着；单关节化后释放。
//     · loc6：静态路径只有 kWind mesh 用它（布料 kRelax 走蒙皮路径，两者互斥）。
//     · loc15 与蒙皮路径的 aInstPosLag（uvec4/LAG 相对坐标）**编号相同但不冲突** ——
//       两条是互斥 program（见下「location 编号只在同一 program 内有意义」）。
//   ⚠️ location 编号只在「同一 draw 的同一 program」内有意义。静态与蒙皮是互斥的
//      program，可各自占用相同的编号而不冲突（二者模型矩阵都落在 7..10 即为例）。
//      本文件只保证「静态路径自身」不越界、不重叠。
//
// 与蒙皮布局的关系：蒙皮布局见 src/instance_buffer.h（kInstance*AttrSpec，loc7..15）。
//   两份 spec 由各自模块拥有、各自演进，共用同一段编号空间但**互不引用**。
//
// 新增一个 per-instance 字段的步骤（维护手册）：
//   1) 在此登记 spec（挑一个空闲槽）、更新上面的预算表、把它加进 kActiveAttrSpecs；
//   2) 在**共享 GLSL 片段**里加 `layout(location = N) in ...`：风相关进
//      instanced_object_renderer.h 的 `kWindBlowGlsl`、颜色相关进 `kInstanceTintGlsl`
//      （三个实例 VS 自动共用，不要往各自的 VS 里抄）。若字段两者都不是，才单独加声明；
//   3) Renderer 加一个 InstanceBuffer（用本文件的 spec），绘制端挂进 InstanceBufferBinding
//      （主 pass / 阴影 pass / 拾取 pass 三处都要挂，否则「树动影不动」）。

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

// 逐实例风向 + 吹倒比例（1 个 vec4）：loc11。
//   打包理由（2026-10-27）：`blow_away_ratio` 原来独占 loc11 的 1 个 float，风向是 vec3
//   —— 两者合一个 vec4 正好，不浪费 attribute 槽（16 个槽是 GL 3.3 的硬地板）。
//   分量：x/y/z = 风向（**世界系单位向量**；渲染端 `CHECK` 长度 ≈ 1）；
//         w     = 吹倒比例（≥0）。
//   VS 里 `layout(location = 11) in vec4 aInstWindDirBlow;`（声明在共享的
//   kWindBlowGlsl 里，三个实例 VS 自动共用）；未吹风时整段不挂载。
//   吹倒位移 = w × L × (L/H)^(n−1)（n = WindCommand::blow_away_exponent），
//   方向 = 风向的**水平分量**；L = 顶点到 root 的父链总长度（VS 沿关节链现算）。
inline constexpr InstanceAttrSpec kWindDirBlowAttrSpec{/*base_loc*/ 11, /*slot_count*/ 1,
                                                       /*slot_components*/ 4, /*stride*/ 4};

// 逐实例风参数（1 个 vec4）：loc15。
//   分量：x = 相位偏移（**秒**，整条风时间轴平移）；y = 摆幅倍数（≥0，默认 1）；
//         z = 频率倍数（>0，默认 1）；w = 阵风相位偏移（**周期比例** [0,1)，默认 0）。
//   物理定义（含典型取值与量纲）见 interface/render_command.h 的 InstanceState。
//   默认值必须是**逐位无操作**（0 / 1 / 1 / 0）—— 现有 gold 才能逐像素不变。
inline constexpr InstanceAttrSpec kWindParamsAttrSpec{/*base_loc*/ 15, /*slot_count*/ 1,
                                                      /*slot_components*/ 4, /*stride*/ 4};

// 逐实例 tint 调色板（2026-10-27）：两个 RGB 颜色 + 一个开关，loc12/13/14。
//   与蒙皮路径的 loc12 misc 里的 RGB888 不同：静态路径有空槽，直接传 float（无量化）。
//   enabled=0（默认）⇒ 片元整段跳过；颜色分量 [0,1]（host 侧 clamp，与蒙皮路径一致）。
inline constexpr InstanceAttrSpec kTint0AttrSpec{/*base_loc*/ 12, /*slot_count*/ 1,
                                                 /*slot_components*/ 3, /*stride*/ 3};
inline constexpr InstanceAttrSpec kTint1AttrSpec{/*base_loc*/ 13, /*slot_count*/ 1,
                                                 /*slot_components*/ 3, /*stride*/ 3};
inline constexpr InstanceAttrSpec kTintEnabledAttrSpec{/*base_loc*/ 14, /*slot_count*/ 1,
                                                       /*slot_components*/ 1, /*stride*/ 1};

// 本路径当前启用的 per-instance spec 清单（绘制端据此挂 buffer；测试据此做全量自检）。
//   新增字段时把新 spec 追加进来。
inline constexpr InstanceAttrSpec kActiveAttrSpecs[] = {
    kModelAttrSpec,        // 7..10
    kWindDirBlowAttrSpec,  // 11
    kTint0AttrSpec,        // 12
    kTint1AttrSpec,        // 13
    kTintEnabledAttrSpec,  // 14
    kWindParamsAttrSpec,   // 15
};

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
static_assert(SpecIsValid(kWindDirBlowAttrSpec),
              "静态实例风向+吹倒 spec 非法：越 16 或 stride 不符");
static_assert(SpecAvoidsVertexAttrs(kWindDirBlowAttrSpec),
              "静态实例风向+吹倒 spec 与顶点属性槽重叠");
static_assert(SpecsDisjoint(kModelAttrSpec, kWindDirBlowAttrSpec),
              "静态实例模型 spec 与风向+吹倒 spec 槽位重叠");
static_assert(SpecIsValid(kWindParamsAttrSpec),
              "静态实例风参数 spec 非法：越 16 或 stride 不符");
static_assert(SpecAvoidsVertexAttrs(kWindParamsAttrSpec),
              "静态实例风参数 spec 与顶点属性槽重叠");
static_assert(SpecsDisjoint(kWindParamsAttrSpec, kModelAttrSpec) &&
                  SpecsDisjoint(kWindParamsAttrSpec, kWindDirBlowAttrSpec) &&
                  SpecsDisjoint(kWindParamsAttrSpec, kTint0AttrSpec) &&
                  SpecsDisjoint(kWindParamsAttrSpec, kTint1AttrSpec) &&
                  SpecsDisjoint(kWindParamsAttrSpec, kTintEnabledAttrSpec),
              "静态实例风参数 spec 与既有槽位重叠");
static_assert(SpecIsValid(kTint0AttrSpec) && SpecAvoidsVertexAttrs(kTint0AttrSpec),
              "静态实例 tint0 spec 非法");
static_assert(SpecIsValid(kTint1AttrSpec) && SpecAvoidsVertexAttrs(kTint1AttrSpec),
              "静态实例 tint1 spec 非法");
static_assert(SpecIsValid(kTintEnabledAttrSpec) &&
                  SpecAvoidsVertexAttrs(kTintEnabledAttrSpec),
              "静态实例 tint 开关 spec 非法");
static_assert(SpecsDisjoint(kTint0AttrSpec, kModelAttrSpec) &&
                  SpecsDisjoint(kTint0AttrSpec, kWindDirBlowAttrSpec) &&
                  SpecsDisjoint(kTint1AttrSpec, kModelAttrSpec) &&
                  SpecsDisjoint(kTint1AttrSpec, kWindDirBlowAttrSpec) &&
                  SpecsDisjoint(kTint0AttrSpec, kTint1AttrSpec) &&
                  SpecsDisjoint(kTintEnabledAttrSpec, kTint0AttrSpec) &&
                  SpecsDisjoint(kTintEnabledAttrSpec, kTint1AttrSpec) &&
                  SpecsDisjoint(kTintEnabledAttrSpec, kModelAttrSpec) &&
                  SpecsDisjoint(kTintEnabledAttrSpec, kWindDirBlowAttrSpec),
              "静态实例 tint spec 与既有槽位重叠");

}  // namespace instanced_object
}  // namespace jpov

#endif  // JPOV_INSTANCED_OBJECT_ATTRS_H_
