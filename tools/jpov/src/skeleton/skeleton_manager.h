// JPOV skeleton — 骨架蒙皮子系统 GL 层：SkeletonManager（资源持有）声明
//
// src/skeleton/ 与 interface/skeleton_types.h 对应：CPU 侧类型(GL-free)在 interface，
// 真正碰 GL 的上传/持有在这里，仿照 src/object3d/（其 CPU 侧在 interface/render_command.h +
// interface/pbr_material.h，GPU 的 DrawObject3D 在 src/object3d/object3d_renderer.h）。
//
// SkeletonManager 职责（对照架构文档 docs/jpov_crowd_instancing_arch.md §6.2-B / §8 #2）：
//   - 接收 CPU 的 SkeletonTemplate（拓扑 + rest 平移），上传该骨架的**逆绑定矩阵**
//     constant（蒙皮用之；随骨架不变、不随动画帧走，故单独存，不塞进逐帧骨骼纹理）。
//   - 接收 SkeletonClip（烘焙源），把每帧每骨头 JointMatrix 烘焙成一张 RGBA 骨骼动画纹理
//     （纹理每"行"=一帧、每"列"=某骨某根 4x4 的 4 个 texel；见 skeleton_types.h 顶部物理链路）。
//     运行期实例只送 clip_id + phase，蒙皮 VS 查骨骼纹理并 4-bone lerp。
//   - 分配 skeleton_id / clip_id 句柄，供 RenderCommand 里的 DrawMeshWithSkeleton 引用。
//
// 多骨架 = 多实例：一套 SkeletonManager（或 SkeletonManager 内多份 template record）管一份
// 骨架；人和马只是各自 RegisterSkeleton 一份 template + 各自的 clip → DrawMeshWithSkeleton
// 引用各自的 skeleton_id。骨架与物种无关是本类不叫 human 的原因。
//
// ⚠️ 本文件当前为 **framework 声明阶段**：只给出可供 review 的接口与所有权模型，
// 具体的 GL 上传 / 烘焙 / instanced 蒙皮实现标 TODO，待实现 PR 落地（见各方法注释）。

#ifndef JPOV_SRC_SKELETON_SKELETON_MANAGER_H_
#define JPOV_SRC_SKELETON_SKELETON_MANAGER_H_

#include <cstdint>

#include "tools/jpov/interface/skeleton_types.h"

namespace jpov {

// ==================== 骨骼动画纹理的生产者 ====================

// 骨骼动画纹理烘焙结果（GL 资源），归 SkeletonManager 持有：一张 RGBA 纹理按
// (col: bone 4x4, row: frame) 存各骨头 JointMatrix。(真正上传进显存在实现 PR)
//
// TODO(2026-09-06): 此 struct 目前是所有权模型的占位，实现时再决定字段(gl_tex、
// bone_count/frame_count、采样选项)放这里还是放 manager record 内。
struct SkeletonAnimTexture {
    // TODO(2026-09-06): GLuint gl_tex; int bone_count; int frame_count; ...
};

// ==================== SkeletonManager ====================

// 一个骨架的 GPU 资源持有者。RegisterSkeleton 登记一份 template、RegisterClip 给它挂
// 一段动画(烘焙)，之后 renderer 用一个骨架复用同一批 rest mesh / 逆绑定 / 骨骼纹理把
// 一批实例 instanced draw 掉。
class SkeletonManager {
public:
    SkeletonManager() = default;
    ~SkeletonManager() = default;  // framework 阶段无 GL 子资源；实现 PR 再改为真正释放。

    SkeletonManager(const SkeletonManager&) = delete;
    SkeletonManager& operator=(const SkeletonManager&) = delete;

    // RegisterSkeleton: 登记一份骨架模板，返回 skeleton_id（供 RenderCommand 引用）。
    //
    //   上传：按 tpl 拓扑算/存每关节逆绑定矩阵 constant（GL 资源在实现 PR，本方法当前
    //   只登记元数据并分配 id）。
    //
    // Pre-condition: tpl.Validate() 已通过。
    // TODO(2026-09-06): GL 上传逆绑定 constant；同 tpl 重复登记是否去重待定(参照
    // TextureManager::LoadFromFile 去重 vs Register 不查重，取后者=调用方保证不重复登记)。
    uint32_t RegisterSkeleton(const SkeletonTemplate& tpl);

    // RegisterClip: 给某骨架挂一段动画 clip，烘焙成骨骼动画纹理，返回 clip_id。
    //
    //   将 clip 每帧解成每骨头 JointMatrix → 烘焙进 SkeletonAnimTexture(clip 与骨架解耦，
    //   但需同模板一起烘焙)。运行期实例送 clip_id + phase。
    //
    // Pre-condition: skeleton_id 已 RegisterSkeleton。
    // TODO(2026-09-06): 烘焙实现；clip 的 CPU 帧数据入口(SkeletonClip 目前只有帧元数据)见
    //   interface/skeleton_types.h 里 SkeletonClip 注释(TODO: 数据容器归属动画源/骨架侧)。
    uint32_t RegisterClip(uint32_t skeleton_id, const SkeletonClip& clip);

    // ReleaseSkeleton / ReleaseClip: 释放本 manager 持有的 GL 资源。
    // 不存在的 id → 静默忽略(允许重复释放)。
    // TODO(2026-09-06)
    void ReleaseSkeleton(uint32_t skeleton_id);
    void ReleaseClip(uint32_t skeleton_id, uint32_t clip_id);

    // 让某骨架的可采样骨骼动画资源就绪(把骨骼纹理 bind 到某纹理单元, 供渲染该骨架的批次;
    // 若有多 clip:VS 需能区分取哪一分页, 见实现抉择)。
    // TODO(2026-09-06): 目前单骨架/静态 rest 时这一步可退化为 "仅把逆绑定传好, 不 bind clip",
    //   即 DrawMeshWithSkeleton 做纯静态蒙皮(零动画)也成立 —— 是 S0 "rest 姿态退化为现有
    //   静态不带蒙皮 VS=零回归门" 的第一个里程碑(见 render_command.h DrawMeshWithSkeleton)。
    //   多 clip / instancing 批次绑定细节在实现 PR 定。

    // ---- 元数据查询(给 renderer / 测试) ----
    // TODO(2026-09-06): GetBoneCount(id), 查询逆绑定是否显式给过等, 实现时按需补。

private:
    uint32_t next_skeleton_id_ = 1;   // 0 = 无效 skeleton_id
    uint32_t next_clip_id_ = 1;       // 0 = 无效/rest
    // TODO(2026-09-06): 骨架/clip 的 GL 子资源(逆绑定 VBO/UBO、骨骼动画纹理)的记录容器
    //   (如 skeleton_id → SkeletonRecord) 在实现 PR 落；本 framework 只把句柄分配接口立好。
};

}  // namespace jpov

#endif  // JPOV_SRC_SKELETON_SKELETON_MANAGER_H_
