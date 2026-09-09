# JPOV — 骨架蒙皮渲染 · M1 设计（基础 shader 单 pose 采样链路）

> 本 PR 把「蒙皮渲染」的第一块落地：**单 pose 静态采样的基础蒙皮 shader 链路**。范围刻意收窄
> 到最小可验证的一步（M1）：用一份 **bind pose（T-rest）** 驱动一个 rest 蓝人网格，走真正
> 的蒙皮 VS（读 JOINTS/WEIGHTS + pose atlas）→ 复用 object3d PBR 片元 → 渲出 T-pose 蓝人，
> 并给出 `human_skeleton_gold_t_rest` 的 gold（sunny-day）验收。动画（两 pose 插值）、
> instancing、FBX 动画 pose、以及蒙皮进 shadow/picking/highlight 属后续（见「边界」）。
>
> 配套文档：`docs/jpov_skeleton_manager_design.md`（SkeletonManager / pose atlas 方案甲）。

成立日期：2026-09-09（Danis 与 agent 对齐后定稿，勿当论文，按此实施）。

---

## 1. 目标（M1）

把一个 rest 蒙皮网格（带 `JOINTS_0/WEIGHTS_0`）经 SkeletonManager 的 pose atlas 做**单 pose
静态**蒙皮渲染，sunny-day 光照，输出应与「非蒙皮直画该 rest 网格」**几乎一致（diff≈0）**。

这第一步只求打通并证明正确的链条：
`顶点(joints/weights) → atlas 肤矩阵 → Σ w·M·v →（与 object3d 同光照）→ 蒙皮位姿`
之后才谈动画（两 pose 插值）/ instancing / 动画源重采样。

## 2. 现状（这块其实大部分已 ready）

| 层 | 现况 |
|---|---|
| 蒙皮 rest mesh | ✅ `LoadGltf`/`LoadGltfScene` 已交付带 `joint_indices/weights` + `kJoints` 的 mesh（顶点留在 mesh/bind 空间，不套 node 父链旋缩，见 gltf_loader.cc 注） |
| SkeletonType（树 + inverseBind） | ✅ `LoadGltfSkeleton` 从 skin 读 23 关节 mixamorig 树 + inverseBind（不含 pose） |
| pose（bind pose / T-rest） | ⚠️ **不在 loader 里反解**（mesh 顶点未必覆盖全部 23 骨 → 反解不完整 = 半套 pose，难理解）。用**标准硬编码 cover**：`MakeMixamoBindPose()` |
| SkeletonManager（pose atlas GPU） | ✅ 已实现（方案甲：inverse_bind 烘焙期折入 atlas 行，见 skeleton_manager.cc） |
| mesh GPU 上传 loc3(joints int)/loc4(weights float) | ✅ mesh_manager 已就位 |
| 蒙皮 shader | ⚠️ 本 PR 落（VS 改蒙皮；FS 复用 object3d PBR 片元） |
| renderer 派发（kSkinnedMesh / DrawMeshWithSkeleton） | ⚠️ 后续实现 |

## 3. 数据流（M1）

```
glb(mixamo_male.glb)
   ├─ LoadGltf*        → MeshData{positions/normals/uv/tangent + joint_indices + joint_weights + kJoints}
   └─ LoadGltfSkeleton → SkeletonType{joints(23: root,parent,rest_offset), inverse_bind}
                                                          │ 只有 joint 信息 + inverseBind，无 pose
标准 cover: MakeMixamoBindPose() ─────────────────────────┘→ SkeletonPose(bind)  [T-rest 模板]
SkeletonManager(SkeletonType, {bindPose})
   → 沿树 jointWorld(bind)= T(rest_offset)·R(joint_rotation)
   → × inverseBind = I   ⇒ 该 pose 的肤矩阵=单位 ⇒ 蒙皮=原 rest 网格 ⇒ M1 diff≈0 成立
   → 烘焙成 pose atlas（一行 = 一个 pose 的 23 骨 4×4）
蒙皮 VS: 每顶点 loc3/4 → Σ w_i · atlas(pose,bone_i) 的肤矩阵 · rest_pos（mesh 局部空间蒙皮）
        → 再乘 uModel(center/up/front/scale) 到世界（法线/切线同用蒙皮后 mat3）
FS: kMeshFs3dPBRFull（与 object3d 相同，sunny-day 光照照抄，不改）
renderer: RegisterSkeleton(SkeletonType, poses)→skeleton_id；DrawMeshWithSkeleton(mesh_id,skeleton_id,instances)
```

## 4. 蒙皮 VS（本 PR 的 shader 部分）

`src/skinning/skinned_mesh_renderer.h` → `kSkinnedVs`。

- 输入沿用 object3d PBR 完整版：`aPos(0)/aNormal(1)/aTexCoord(2)/aTangent(5)`；
  另读 `aJoint(3, ivec4)`、`aWeight(4, vec4)`（mesh_manager VBO 已备好 loc3/4）。
- uniform：`uMVP`、`uModel`（同 object3d）+ `uPoseAtlas`(sampler2D RGBA32F)、`uBoneCount`、
  `uPoseRow`、`uPoseCol`（本实例该 pose 在 atlas 的行/列起点，CPU 已 divmod 好）。
- 输出与 `kMeshVs3dPBRFull` **完全一致**：`vWorldPos / vWorldNormal / vTexCoord / vWorldTangent`
  → 因此 FS 直接复用 `kMeshFs3dPBRFull`（同光照，片元 0 改动）。
- 蒙皮：在 mesh 局部空间做 4-bone 加权累加
  `sp = Σ w_i·(M_i·aPos)`，`sn = Σ w_i·mat3(M_i)·aNormal`，`st = Σ w_i·mat3(M_i)·aTangent`；
  然后 `vWorldPos=(uModel·sp).xyz`、`vWorldNormal=normalize(mat3(transpose(inverse(uModel)))·sn)`、
  `vWorldTangent` 同理、`vTexCoord=aTexCoord`、`gl_Position=uMVP·vec4(sp,1)`。
- **矩阵还原**：atlas 里每 (pose,bone) 存的是**行主序** 4×4（SkeletonManager PutMat4Row：
  bone 占 4 连续 texel，texel t 存矩阵第 t 行）。从 4 个 texel 还原成 GLSL `mat4`（列主序存储）
  需按行主序元素重排（`col0=(m00,m10,m20,m30)`…见 `LoadBoneMatrix` 注释，防错序）。
- **bind pose 一致性**：绑定时 M_i=I ⇒ sp=aPos、sn=aNormal、st=aTangent，与 kMeshVs3dPBRFull
  逐位一致 ⇒ M1 diff≈0。

### 坐标 / 单位
- 蒙皮发生在 **mesh 局部空间**（loader 已刻意把 bind 顶点留在此，不套 node 旋缩），
  这正是 skin 关节矩阵 + inverseBind 作用的空间。
- `LoadGltfSkeleton` 还原的 SkeletonType 与蒙皮网格**同构**（同 skin.joints 顺序 / 同树 /
  同 rest_offset），故 joint 索引与顶点 loc3 的 skin 关节序号对齐。
- 坐标轴归一是渲染/放置层的事（JPOV 模型局部 up=+Y，见 render_command.h）；loader 不在此
  阶段转轴（与现有 gltf loader 一致）。

## 5. bind pose cover（T-rest 模板）

`src/skinning/mixamo_bind_pose.h` → `MakeMixamoBindPose()`。

- 返回 `SkeletonPose`（bone_count=23，`joint_rotation[i]` = 每骨 local 旋转四元数 xyzw）。
  数值从 mixamo_male.glb 每关节 node.local.rotation 原样摘出（含 `Root` 轴修正 -90°X）。
- `rest_offset`（骨长/平移）不在此重复 —— 由 `LoadGltfSkeleton` 生成的 SkeletonType 提供，
  此 pose 只补「旋转」这一半（与 SkeletonType 同构）。
- 为何 cover 而非反解：mesh 顶点未必覆盖全部 23 骨（某些骨无顶点/权重=0）→ 从 mesh 反解
  bind pose 会得到**半个 pose**，难理解、易误导。M1 只需一份**已知、完整**的 bind pose 当模板
  把链路跑通，故硬编码 cover（后续健全后再把干净提取替换过来，并用 FBX 覆盖单测）。

## 6. renderer 集成（后续实现，本 PR 只定向方案）

- JPOV 新增：`RegisterSkeleton(SkeletonType, poses) → skeleton_id`；
  `RenderCommandList` 已有 `SkinnedMeshCommand`/`cmds.skinned_mesh`/`DrawMeshWithSkeleton(...)`。
- Renderer 持有 `skeleton_id → unique_ptr<SkeletonManager>` + id 分配（SkeletonManager 无自管 id，
  renderer 持映射，见 skeleton_manager.h 所有权 v3）。
- 主 pass `Draw3DCommands` 加 `case kSkinnedMesh` → `DrawMeshWithSkeleton`：
  绑定蒙皮 program `{kSkinnedVs, kMeshFs3dPBRFull}`；**光照复用** `Object3DRenderer::
  UploadSunData/UploadAmbient`（把蒙皮 program 作为 both prog 上传），必要时补点光/tile 光照；
  绑 pose atlas 纹理（GpuHandles.pose_atlas_tex）+ `uBoneCount/uPoseRow/uPoseCol`；设 uMVP/uModel、
  纹理贴图（同 DrawObject3D）；绑 mesh VAO（mesh_manager 已含 vbo_joints/weights loc3/4）；draw。
- M1 只做**单实例、单 pose（pose_a==pose_b=0）**，不铺 instancing、不做两 pose 插值。

## 7. gold test（本 PR 的验收）

- 目录：`tools/jpov/test/skeleton/`（独立于 object3d —— 这是单独 shader 路径，不复用 object3d
  的 test 目录）。
- 名字：`human_skeleton_gold_t_rest_generator` / `human_skeleton_gold_t_rest_test`
  （生成 PNG `.../skeleton/human_skeleton_gold_t_rest_1280x720.png`）。
- generator：加载 mixamo_male.glb 的 rest mesh + LoadGltfSkeleton 得 SkeletonType +
  `MakeMixamoBindPose()` 得 bind pose → `SkeletonManager(type,{bindPose})` → RegisterSkeleton →
  DrawMeshWithSkeleton(单个 T-rest 蓝人) → sunny-day（太阳平行光 + CSM + ambient，同
  std scene_in_sun 那套）→ 写 gold PNG。
- test：渲一帧到 temp，与库内 gold 比对。因 PBR/llvmpipe 三稳态非确定（同现有 object3d PBR
  gold 的决策），**不做逐像素颜色比对**；正确性由 generator 产出的图供 Danis 肉眼确认 +
  渲染链路跑通 smoke check（gold 存在 + 非平凡输出）。另可选：同场景骨**bind pose 蒙皮** vs
  **非蒙皮直画** 输出 diff≈0 的辅助断言（证明链路对）。

## 8. 边界 / 不做（留后续）

- 两 pose 插值（pose_a/pose_b/ratio）与 instancing —— 下一 PR（M3）。
- FBX 动画 pose / 重采样喂骨架 —— 后续（基于 FBXClip 二次开发）。
- 蒙皮进 shadow / picking / highlight 四 pass 同一套 —— 后续（render_command.h 已注明）。
- 从 mesh 干净反解 bind pose（替换 cover）+ Fbx 覆盖单测 —— 后续。
- 骨骼网格未覆盖骨（半 pose）的处理 —— 因走 cover，M1 不涉及。

## 9. 关键决策（对齐 Danis）

- 蒙皮只在 **VS 改**（读 joints/weights + atlas 肤矩阵）；**FS 复用 object3d PBR 片元**（surny-day
  光照照抄老 shader，不重写）。
- **glb load 不出 pose**：只给 joint 信息 + inverseBind；bind pose 靠 `MakeMixamoBindPose()` cover。
- M1 用**单 pose（T-rest）静态**链路做第一个可验证门（diff≈0），不铺动画/instancing。
- `human_skeleton_gold_t_rest` 独立于 object3d 目录（单独 shader 路径）。
