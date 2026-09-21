// JPOV — FBX 动画/骨架加载器：.fbx → FBXClip（CPU，GL-free）
//
// 用已 vendor 的 third_party/ufbx-static（单文件完整 FBX 库）读外部 DCC 导出的动画文件，把
// 「源骨架 + 动画原始全帧 + 帧时序」整段抓进 interface/FBXClip，供:
//   (a) debug —— 装一个源骨架（mixamorig 之类命名 + 层级），肉眼/工具比对它与 JPOV
//      自造骨架是否一致（集成效果不对时看命名/层级对没对上）；
//   (b) 后续 retarget / resample —— 基于 clip 二次开发（本文件不做重定向、不做重采样）。
//
// 设计要点（与 Danis 对齐的现阶段范围，勿过度）:
//   - 骨架加载只求 **bone type/层级 清晰可见**，**不做任何 check**（不 Validate 拓扑、
//     不要求与某模板骨数一致、不对骨名做 sanity）。SkeletonType::Validate() 是另一些调用
//     点（SkeletonManager 构造 / user 想拿它当真骨架去建 SkeletonManager）的事，本 loader
//     读取路径不碰 —— 文件里有啥就装啥，缺了/命名怪都照搬，留给 debug 肉眼看。
//   - 动画 **取源原始所有帧 + 必需时序**，不重采样、不裁剪、不插值。源文件以 fps 等间隔
//     落 key（如 Mixamo 30fps，key 逐个在帧网格上），本 loader 逐帧在帧时间是点上取每骨
//     local 旋转即得原始帧（关键帧恰落网格 → 无插值）。resample 留给基于 clip 的二次开发。
//   - pose 语义（2026-09-14 修复）：帧里存的是**相对 bind 的增量旋转**（= 源每帧 Lcl
//     Rotation 的语义），不是全量 local 旋转 —— 详见 LoadFbxAnimation 注释。
//
// 单位铁律（2026-09-20 定稿）:
//   **加载边界是唯一的长度换算点。** 两个入口都把长度量乘上源文件 unit_meters，输出**米**。
//   故本 loader 出去的一切（joints[].rest_offset / frames[].root_offset）都是米，与 glTF 骨架、
//   Mixamo23 模板、场景地面同尺度 —— 消费方**不需要、也不应再做任何换算**。
//   反面教训（2026-09-20 实修）：旧行为是本入口“原样透传”源单位（clip = cm）而本文件另一入口
//   LoadFbxSkeleton 已换米 —— 两个入口尺度不同，重定向按“无量纲骨长比”缩放时**静默错 100 倍**，
//   角色“飞走” 63 米。故不是“两个入口刻意分工”，而是“两个入口必须同尺度”。
//
//   JPOV 是 y-up（+Y 上）。但在本 loader 产出的数据层，姿态存的是**每骨相对父的旋转**
//   （SkeletonPose.joint_rotation）+ 根位移(root_offset) —— 这种相对父的表达天然与全局轴
//   无关：角色最终朝上哪个方向由【放置层】(SkinnedInstanceState 的 up/front、外置 model
//   matrix) 决定，不锁在 pose 里。故本 loader 不额外把源 FBX 的轴“硬旋”进 pose —— 那是
//   放置层才有的语义（Minimal Surprise：不把需要整体骨架旋转的事情藏进每骨姿态）。
//   具体：源 FBX 的 axes **原样透传**进 clip（与 glTF loader 透传原生轴一致；本 loader 不
//   把方向“硬转”进 pose）。常见的 Mixamo 人形源默认 y-up → 传过即已 y-up，无需旋转。
//   成功 LOG 会带出源 scene.axes.up 与 unit_meters，便于 debug 手头这份是不是 y-up。
//   ⚠️ **长度单位则相反：在加载边界一律换成米**（两个入口都是）。JPOV 内部不流通
//   厘米制数字，详见下方「单位铁律」。
//   （若某源是 z-up 等非 y-up 且要“立起来渲染”，在其放置层按 up/front 对齐即可 ——
//   那层有角色朝向，不在本 loader。同 gltf_loader：加载路径一律不改方向。）

#ifndef JPOV_SRC_FBX_LOADER_H_
#define JPOV_SRC_FBX_LOADER_H_

#include <string>

#include "tools/jpov/interface/animation_clip.h"

namespace jpov {

// 从 FBX 文件加载一段动画 + 其内建骨架到 FBXClip。
//
//   path  : .fbx 文件路径。
//   out   : 非空则填充 {源骨架(在 mixamorig: 等命名 + bind 静止偏移), 帧频, 全帧}。
//   长度量一律为**米**（见上方「单位铁律」）。
//   返回 false 表示加载/解析失败（路径不存在 / ufbx 读不了 / 无动画可载），此时 out 不清。
//
// 语义 / 约束:
//   - 只收集**带 bone(Bone 属性, 即 Skeleton node)的 node 子树**当骨架 —— 无 bone 的空
//     Null / 网格 / 相机 node 不入骨集。collect 顺序 = 沿 node 树深度优先 → 天然拓扑序
//     （父先于子树，见 SkeletonType::Validate 的拓扑要求）。根骨(通常 mixamorig:Hips)
//     无父 → parent = kSkeletonNoParent。
//   - SkeletonType.joints[i]:
//       .name = 该骨的 ufbx 名(原样)；.parent = 沿树向上第一个也同为 bone 的节点索引；
//       .rest_offset = 该骨 bind/静止 的 local translation（相对父）× source_unit_meters
//       → **米**（与 LoadFbxSkeleton 同一规则）；用它让骨架能按真实骨长/朝向展开，
//       供 debug 观察层级形状，并与 glb 骨架同尺度对比。
//   - 每个 SkeletonPose（= 一帧）:
//       .joint_rotation[i] = 该骨在该帧、**相对 bind 的增量旋转**四元数（单位）。
//         实现：q_delta = R(bind_rotation[i])⁻¹ ⊗ q_full（q_full = ufbx evaluate 的该时刻
//         全量 local 旋转）。即"源每帧 Lcl Rotation"的语义：identity pose ⇒ 源在
//         Lcl Rotation=0 时的静止形态（Mixamo 源为 T-pose）。
//         ⚠️ 2026-09-14 修复前直接存 q_full（含静态 bind 朝向）→ 与烘焙式
//         jointLocal = T(rest_offset)·R(bind_rotation)·R(pose) 里的 R(bind) 双倍施加。
//       .root_offset    = 根骨在该帧、**相对 bind 位置**的平移量（root-motion），在根的
//         **父坐标系**下表达（“根是顶层骨”的源即模型系）；静息恒 0。
//         = (当前 local 平移 − bind 位置 local 平移) × source_unit_meters → 与 rest_offset
//         同为**米**（源单位下相减，最后一次性换算）。
//         ⚠️ 2026-09-20 修：此前存**全量** local translation，静息 ≈ 一个腰高而非 0 ——
//         下游当增量用会把角色整体抬高一个腰高（悬空）。定稿见
//         docs/jpov_root_offset_design.md §1。
//         根骨之外每骨平移若也被源动画驱动（少见），现阶段只取旋转（rest 平移由 skeleton
//         静止形状给）——超出 debug 范围。
//   - frames_per_second = scene settings 的 fps；frames 数量 = floor((end-begin)*fps)+1
//     （0 起点，如 17.2333s@30fps → 518 帧：k=0..517，t_k=k/30）。
//   - 不做重定向/重采样/播放控制 —— 那是上层与本 loader 无关的后续演变。
bool LoadFbxAnimation(const std::string& path, FBXClip* out);

// 从 FBX 文件只加载其内建骨架（不读动画帧）。
//
//   path  : .fbx 文件路径。
//   out   : 非空（CHECK 保护）；成功时填充骨架。
//   返回 false 表示加载/解析失败（路径不存在 / ufbx 读不了 / 无 bone 节点），此时 out 不清。
//
// 定义（与"从 glb 读骨架" LoadGltfSkeleton 同构，本函数是 FBX 侧的对应物）：
//   - joints[i].rest_offset   = 该骨 bind(静止) local 平移 × unit_meters → **米**。
//     （与 glb 侧（glTF 原生米）、Mixamo23 模板同尺度，保证"两个资产骨人"能同场对比 /
//       后续重定向单位一致；也与 LoadFbxAnimation 同规则 —— 见上方「单位铁律」。）
//   - joints[i].bind_rotation = 该骨 bind(静止) local 旋转（Mixamo 源 = PreRotation 的合成；
//       FBX 的 Lcl Rotation 是动画通道、默认≈0，静止朝向存在 PreRotation 里）。
//   - 该骨架在 **identity pose** 下的完整形态 ≡ 源文件在「**Lcl Rotation = 0**（无动画）」时
//     求出的姿态（Mixamo 官方源为 T-pose）。
//
// 范围/约束：收集全部带 bone 属性的节点（Mixamo 全身含手指 → 65 骨），不做 Validate、
//   不做骨名 sanity、不自加包装 Root（文件里啥就装啥，懒校验留给消费方）。
bool LoadFbxSkeleton(const std::string& path, SkeletonType* out /*output*/);

}  // namespace jpov

#endif  // JPOV_SRC_FBX_LOADER_H_
