# JPOV Skeleton — SkeletonManager GPU 资源 / pose atlas 布局定稿

> 本文件锁定「骨架蒙皮渲染的 GPU 资源层」的实现契约,交付给 SkeletonManager 实现 PR
> (feature/20260908-jpov-skeleton-preps)以及「为蒙皮 shader 搭建」做准备。CPU 侧数据
> 定义见 `interface/skeleton_types.h`;GL 资源对象骨架见 `src/skeleton/skeleton_manager.h`。
> 配套总架构见 `jpov_crowd_instancing_arch.md`。

成立日期: 2026-09-08(Danis 逐一敲定,勿当论文,照此编码)
> 修订 22:55 方案甲定稿: inverse_bind 不进 GPU 独立存储, 改 CPU 烘焙折入 pose atlas
>   (行业共识: GPU Gems 3 Ch.2 / VAT 教程 — palette.skinningMatrix[j]=globalPose[j]×inverseBind[j]
>    CPU 先乘好, 纹理只存“最终肤矩阵”, shader 每骨一次点采样). 见下方 §3' 方案甲。

---

## 1. 本次 PR(骨架渲染准备)三件事

在真正写蒙皮 shader / renderer 消费 `kSkinnedMesh` 之前,先把三件"食材切好":

1. **gltf_loader 加载 mesh 里的骨骼数据(含上 GPU 侧)**
   读 JOINTS_0 / WEIGHTS_0 accessor → `MeshData.joint_indices[4] / joint_weights[4]`、置
   `MeshVertexFlags::kJoints`;**GPU 上传本身 mesh_manager.cc 已完好**(loc3 用
   `glVertexAttribIPointer(GL_INT)` 传 joints、loc4 用 `glVertexAttribPointer` 传 weights,
   含释放回收),所以 loader 这一步只是把文件里的骨/权重真正填进 `MeshData`。同时从
   glTF `skins[].inverseBindMatrices` + joints node 层级读出骨架 → `SkeletonType`
   (每骨 `name` 取 mixamorig:xxx、`parent` 由 node 树拓扑序给出)。

2. **FBX(Mixamo 动画)→ `vector<SkeletonPose>` 加载**
   用已 vendor 的 `third_party/ufbx-static/`(ufbx 单文件完整库)读 `hip_hop_dance.fbx`:
   沿 `node->bone` 拉出 FBX **内置骨架** → 转 `SkeletonType`;再 evaluate 动画 →
   **不重采样,全帧**铺进 `vector<SkeletonPose>`(每骨 Quaternion 相对父旋转 + root_offset)。

3. **SkeletonManager 内部实现 + GpuHandles 公共句柄接口**
   把 `skeleton_manager.h` 的空壳填实:构造收 `(SkeletonType, vector<SkeletonPose>)` 后
   ① 逆绑定矩阵 → 骨架级 SSBO/UBO(constant) ② 每 pose 沿骨架树烘焙成 JointMatrix →
   平铺上传成 **pose atlas 纹理** ③ 暴露 `GpuHandles` 给 renderer(下个 PR 的蒙皮 shader 用)。
   附:`SkeletonType::Validate()`、CPU 端 pose→JointMatrix 树烘焙工具(可单测)。
   这步**不搭蒙皮 VS、不接 renderer**,只为 shader 把资源层与握手接口铺好。

---

## 2. Pose Atlas 布局(定稿,勿改)

- **纹理类型**:`GL_RGBA32F`,2D,固定 **2048 × 2048**。
- **单个 pose 的尺寸**:每节一个 4×4 矩阵 = 4 个 RGBA texel 排成一列;一行放完某骨架
  全部 bone → pose 宽 = `bone_count × 4` texel。(biped 23 骨 → 92 texel 宽。)
- **每行放几个 pose**:`pose_per_row = floor(2048 / (4 * bone_count))`
  (23 骨 → 22 pose/行)。行尾 2048 − pose_per_row×4×bone_count 的 padding **不使用**。
- **对齐铁律**:pose 宽 `4×bone_count` 恒能整除行宽排布 → **每个 pose 整段落在同一行,
  绝不跨行**(行宽是 pose 宽的整数倍,行尾直接留白),无 fragment 拆分。
- **容量**:单张 atlas `≈ floor(2048²/(4×bone_count))`(23 骨 ≈ 45,590 pose),远超业务
  (现估 ~1800,即 60 动作×30 帧)。
- **高度无 4096 包袱**:因为用 2D 平铺而非"一行一 pose 的高窄条",capacity 不撞
  `GL_MAX_TEXTURE_SIZE`/FBO 维度的保守上限,海量 pose 也无压力。

---

## 3. 数据分工 —— 方案甲定稿(2026-09-08 22:55)

原 §3 初版曾写 "pose 存相对根的 JointMatrix × SSBO 里的 inverse_bind"(即 inverse_bind 独立
constant)。但查工程 GL 层后发现 **SSBO/UBO 在本 Project 不可移植**(MinGW gl_loader 无
`glBindBufferBase`/任何 SSBO 别名;Linux 也是裸 `<GL/gl.h>`+glibc 原型, 无 glew/glad;现成只有
glBindBuffer/glBufferData 当 VBO/EBO、glTextureData、glTexSubImage2D、glUniformMatrix4fv)。
故走**方案甲 = 业界把 inverse_bind 折进 pose bake 的主流做法**:

### 方案甲 — inverse_bind 由 CPU 在烘焙期折入 pose atlas,不进 GPU 独立存储
- **蒙皮所需的“最终肤矩阵”** baked 成 atlas 每 pose 每骨:
  `mappedMatrix(bone, pose) = jointWorld(bone, pose) × inverseBind(bone)`
  (CPU 沿骨架树解出每骨相对角色根的 jointWorld 后, 乘上骨架级逆绑定, 再落 atlas 行)。
- GPU 只持有**一张 pose atlas 纹理**(RGBA32F 2048²), 无独立 inverse_bind 资源。蒙皮 VS 每骨
  只要**一次点采样** atlas 得该 (pose, bone) 的最终矩阵, 直接 Σ weight·M·v; 无需再读
  inverse_bind(已被 CPU 乘进去)。
- 这正是 GPU Gems 3 Ch.2 / 各 VAT 教程的 `palette.skinningMatrix[j]=globalPose[j]×inverseBind[j]`。
- inverse_bind 作为**骨架级 CPU 持有**(构造时传入 / LoadGltfSkeleton 读出), 只用于烘焙。
- 若某骨架未带 inverse_bind(手写/单位), 烘焙时按需用单位阵或沿树自算 rest 逆 —— 现状
  保留可选覆盖(见 skeleton_types.h SkeletonType.invserse_bind 为空语义)。

| 数据 | 归属 | 载体 | 说明 |
|---|---|---|---|
| **inverse_bind** | 骨架级 CPU 持有 | 不单独上 GPU | 只作烘焙乘数, 折入 atlas 后 render 不见它 |
| **映射后的最终肤矩阵**(= jointWorld×inverseBind) | 骨架级烘焙成 **pose atlas** | RGBA32F 2D atlas(§2) | CPU 先乘好 inverse_bind; instance 以行号引用 |
| 该 instance 用哪个 pose / 插值 | **per-instance** | per-instance attribute | 传 pose (row,col) offset + ratio, 不给矩阵 |

**蒙皮链(方案甲)**:per-instance 行号 → atlas 点采样得 (pose,bone) 最终肤矩阵 M →
Σ weight·(M×v)。VS 不需另一处取 inverse_bind(y CPU 已折入)。

⚠️ 注:运行时若要在两个 pose **之间**插值(prace ratio),先各自从 atlas 取 pose_a/pose_b 的
矩阵、shader 里逐骨 lerp 两矩阵后再蒙皮 —— 语义与 09-07 双 pose 插值一致(都是对肤矩阵 lerp)。

### 关键优化(成本定论,已消的担忧)
- **行列 offset 是 per-instance,不是每顶点**:1000 个 instance 只有 1000 组 pose 引用,
  故把 `pose_idx―(row,col)` 的 divmod 在 **CPU 端每 instance 一次**算好、作为 per-instance
  整数行列传进 VS;**VS 全程零除法**。
- **VS 只读该顶点被影响的 ≤4 骨**(loc3 joints 带 bone 索引),非全 23 遍历;每顶点最多
  16 次 RGBA32F texelFetch,layout 无关,不可再砍。
- 2D tile 多付出的仅每 instance 一次 CPU divmod(≈0)+ 更高 2D cache 命中, 换来不撞高度
  上限+大容量, 净赚 —— 定 2D tile(§2)。

---

## 4. 一句话契约(写进 skeleton_manager.h 头注释用语)

> 骨骼动画纹理(SkeletonManager 所有) = 固定 **2048×2048 RGBA32F** pose atlas:
> 行宽容纳 `floor(2048/(4*bone_count))` 个 pose,每个 pose 宽 `4*bone_count` texel
> (每骨一个 4×4 矩阵 = 4 texel),**整 pose 不跨行**,行尾 padding 不使用。atlas 里每
> (pose,bone) 存的是 **最终肤矩阵 jointWorld(pose,bone)×inverseBind(bone)**(CPU 在烘焙期
> 已把骨架级 inverse_bind 折入, GPU 无独立逆绑定资源);蒙皮 VS 每骨一次点采样即得, 直接
> Σ weight·M·v。instance 的每个 pose 用 CPU 端预算好的 **(row,col) 行列 offset** 作
> per-instance attribute 传入,VS 零除法。(方案甲,不采 SSBO —— 工程 GL 层不可移植。)
