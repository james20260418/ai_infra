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
//
// 坐标系 / 单位（与 JPOV 数据模型对齐，2026-09-09 敲定）：
//   JPOV 是 y-up（+Y 上）。但在本 loader 产出的数据层，姿态存的是**每骨相对父的旋转**
//   （SkeletonPose.joint_rotation）+ 根位移(root_offset) —— 这种相对父的表达天然与全局轴
//   无关：角色最终朝上哪个方向由【放置层】(SkinnedInstanceState 的 up/front、外置 model
//   matrix) 决定，不锁在 pose 里。故本 loader 不额外把源 FBX 的轴“硬旋”进 pose —— 那是
//   放置层才有的语义（Minimal Surprise：不把需要整体骨架旋转的事情藏进每骨姿态）。
//   具体：源 FBX 的 axes / 单位 **原样透传**进 clip（与 glTF loader 透传原生单位一致，不做隐式
//   缩放）；常见的 Mixamo 人形源默认 y-up + cm → 传过即已 y-up，无需任何旋转/缩放。
//   成功 LOG 会带出源 scene.axes.up 与 unit_meters，便于 debug 手头这份是不是 y-up。
//   （若未来某源是 z-up 等非 y-up 且要“立起来渲染”，在其放置/retarget 层按源轴语义对齐即
//   可 —— 那层有角色朝向，不在本 loader。）
//   ⚠️ 若读到 gltf_loader 头注释把输出写成 “Z-up”，那是另一条链的术语问题，与本层无冲突；
//   是否顺手改 gltf 注释语义见 PR 说明（本 PR 不作该链渲染行为变更）。

#ifndef JPOV_SRC_FBX_LOADER_H_
#define JPOV_SRC_FBX_LOADER_H_

#include <string>

#include "tools/jpov/interface/animation_clip.h"

namespace jpov {

// 从 FBX 文件加载一段动画 + 其内建骨架到 FBXClip。
//
//   path  : .fbx 文件路径。
//   out   : 非空则填充 {源骨架(in mixamorig: 等命名 + bind 静止偏移), 帧频, 原始全帧}。
//   返回 false 表示加载/解析失败（路径不存在 / ufbx 读不了 / 无动画可载），此时 out 不清。
//
// 语义 / 约束:
//   - 只收集**带 bone(Bone 属性, 即 Skeleton node)的 node 子树**当骨架 —— 无 bone 的空
//     Null / 网格 / 相机 node 不入骨集。collect 顺序 = 沿 node 树深度优先 → 天然拓扑序
//     （父先于子树，见 SkeletonType::Validate 的拓扑要求）。根骨(通常 mixamorig:Hips)
//     无父 → parent = kSkeletonNoParent。
//   - SkeletonType.joints[i]:
//       .name = 该骨的 ufbx 名(原样)；.parent = 沿树向上第一个也同为 bone 的节点索引；
//       .rest_offset = 该骨 bind/静止 的 local translation（相对父，源 FBX 原生单位，如 Mixamo
//       cm）—— 用 node 的 local_transform，让骨架能按真实骨长/朝向展开，供 debug 观察层级形状。
//   - 每个 SkeletonPose（= 一帧）:
//       .joint_rotation[i] = 该骨在该帧时间点的 **local 旋转(相对父)四元数**（取 ufbx 以
//         Euler 序转好的四元数，即 geom::Quaternion 单位四元数）。
//       .root_offset    = 根骨(Hips)在该帧的 local translation（源动作里角色整体位移 /
//         root-motion；静止动作恒 0 附近）。根骨之外每骨平移若也被源动画驱动（少见），
//         现阶段只取旋转（rest 平移由 skeleton 静止形状给）——超出 debug 范围。
//   - frames_per_second = scene settings 的 fps；frames 数量 = floor((end-begin)*fps)+1
//     （0 起点，如 17.2333s@30fps → 518 帧：k=0..517，t_k=k/30）。
//   - 不做重定向/重采样/播放控制 —— 那是上层与本 loader 无关的后续演变。
bool LoadFbxAnimation(const std::string& path, FBXClip* out);

}  // namespace jpov

#endif  // JPOV_SRC_FBX_LOADER_H_
