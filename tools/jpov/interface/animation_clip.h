// JPOV Skeleton — CPU 侧「外部动画源」容器（GL-free）
//
// 用途：承接从外部 DCC 动画文件（当前为 FBX）原样搬进来的**动画素材**，供上层做
//   debug（加载源骨架，肉眼比对命名/层级与 JPOV 自定义骨架是否一致）与后续的
//   重定向（retarget：把某段动作从源骨架映射到目标骨架）二次开发使用。
//
// 本文件只声明动画素材的容器结构，不负责读取（读取在 src/fbx_loader.*）。
// 字段刻意极简，不加无意义的槽位：只保留重建这段素材所必要的信息 ——
//   源骨架（含骨名/层级树，debug/对位凭据）+ 帧频（时序骨架）+ 原始全帧位姿。
//
// ⚠️ 与 interface/skeleton_types.h 的关系：
//   - SkeletonType / SkeletonPose 是 JPOV **自有数据模型**（pose 只驱动每骨旋转，骨架静止
//     形状由 SkeletonType.joints[].rest_offset 给定，见 skeleton_types.h）。
//   - FBXClip 是**横跨两个模型的桥**：它的 skeleton 用 mixamorig: 等外部命名+bind 静止
//     偏移填，frames 用源每帧每骨 local 旋转填 —— 原样倒进 JPOV 的 CPU 容器，
//     不重采样、不裁剪、不二次解释。后续重采样/重定向在「基于本 clip 二次开发」里做，
//     本基本函数不掺。

#ifndef JPOV_INTERFACE_ANIMATION_CLIP_H_
#define JPOV_INTERFACE_ANIMATION_CLIP_H_

#include <vector>

#include "tools/jpov/interface/skeleton_types.h"

namespace jpov {

// 一段外部动画素材（一段「可再用的动作」），从 FBX 之类的 DCC 导出文件整段抓取。
//
// 组成（三件，缺一无法把这段素材当作后续 debug/retarget 的输入）：
//   - skeleton  : 该素材内建的骨架。名称/层级用来做「源骨架 ↔ JPOV 骨架」对位凭据
//                 （第 1 点 debug 目的：加载出来能看见 bone 层级/名字即可，对位是否一致
//                 交给肉眼/上层）。rest_offset 取来源文件的 bind/静止局部偏移（见载入方），
//                 以让骨架能按真实骨长展开。
//   - frames_per_second : 素材定义的帧频。整段动画即在以它为步长的等间隔时间轴上铺帧
//                 （第 i 帧位于 t = i / frames_per_second）。这是二次 resample 的时序依据。
//   - frames    : 原始全帧，一帧一个 SkeletonPose。**不重采样**：直接等间隔把源文件的每个
//                 keyframe 时间点上的每骨 local 旋转取出来填满（源文件 key 恰在帧网格上，
//                 故无需插值）。后续重采样基于本数组二次开发，本基本函数不再做二次加工。
struct FBXClip {
    SkeletonType skeleton;          // 源骨架（骨名 + parent 树 + bind 静止偏移）
    double frames_per_second = 0.0; // 源帧频（fps）；>0 才视为有效 clip。
    std::vector<SkeletonPose> frames;  // 原始全帧位姿，长度 = 这一段动画的帧数。
};

}  // namespace jpov

#endif  // JPOV_INTERFACE_ANIMATION_CLIP_H_
