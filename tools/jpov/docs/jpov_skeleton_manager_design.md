# JPOV Skeleton — SkeletonManager GPU 资源 / pose atlas 布局定稿

> 本文件锁定「骨架蒙皮渲染的 GPU 资源层」的实现契约,交付给 SkeletonManager 实现 PR
> (feature/20260908-jpov-skeleton-preps)以及「为蒙皮 shader 搭建」做准备。CPU 侧数据
> 定义见 `interface/skeleton_types.h`;GL 资源对象骨架见 `src/skeleton/skeleton_manager.h`。
> 配套总架构见 `jpov_crowd_instancing_arch.md`。

成立日期: 2026-09-08(Danis 逐一敲定,勿当论文,照此编码)

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
  (23 骨 → 44 pose/行)。行尾 2048 − pose_per_row×4×bone_count 的 padding **不使用**。
- **对齐铁律**:pose 宽 `4×bone_count` 恒能整除行宽排布 → **每个 pose 整段落在同一行,
  绝不跨行**(行宽是 pose 宽的整数倍,行尾直接留白),无 fragment 拆分。
- **容量**:单张 atlas `≈ floor(2048²/(4×bone_count))`(23 骨 ≈ 45,590 pose),远超业务
  (现估 ~1800,即 60 动作×30 帧)。
- **高度无 4096 包袱**:因为用 2D 平铺而非"一行一 pose 的高窄条",capacity 不撞
  `GL_MAX_TEXTURE_SIZE`/FBO 维度的保守上限,海量 pose 也无压力。

---

## 3. 数据分工(谁在骨架级共享 / 谁 per-instance)

蒙皮所需的每帧骨骼态由两块拼成,**分工不同**:

| 数据 | 归属 | 载体 | 说明 |
|---|---|---|---|
| **inverse_bind**(逆绑定,rest 下每关节相对角色原点逆阵) | **骨架级,全 instance 共享,一次上传** | **SSBO/UBO(constant,非 per-instance)** | 骨架固有不变。绝不让每个 instance 各自背一份(23 骨=1.4KB,若 per-instance×1000 人=1.4MB 纯浪费)。 |
| **pose 的 JointMatrix**(绑定后每帧/每姿态驱动角色根的矩阵) | 骨架级烘焙成 **pose atlas 纹理** | RGBA32F 2D atlas(§2) | 骨架级一次烘焙,instance 只以"行号"引用。 |
| 该 instance 用哪个 pose / 插值 | **per-instance** | per-instance attribute | 传 pose 的**(row, col)行列 offset** + ratio,不给矩阵。 |

**蒙皮链**:per-instance 行号 → atlas 查该 bone 的 pose JointMatrix
× SSBO 里的 inverse_bind × rest 顶点 → Σ weight·(...)。

### 关键优化(成本定论,已消的担忧)
- **行列 offset 是 per-instance,不是每顶点**:1000 个 instance 只有 1000 组 pose 引用,
  故把 `pose_idx→(row,col)` 的 divmod 在 **CPU 端每 instance 一次**算好、作为 per-instance
  整数行列传进 VS;**VS 全程零除法**,蒙皮 body(取 4 bone×4 texel + mat·vec)两布局一致。
- **VS 本来只读"该顶点被影响的 ≤4 骨"**(loc3 的 joints 已带 bone 索引),不是全 23 骨遍历;
  每顶点最多 16 次 RGBA32F texelFetch,这是最小集合,layout 无关,不可再砍。
- 故 2D tile vs 窄行:多付出的仅每 instance 一次 CPU divmod(≈0)+ 更高的 2D cache 命中,
 换来不撞高度上限 + 大容量,净赚 —— **定 2D tile(§2 布局)**。

---

## 4. 一句话契约(写进 skeleton_manager.h 头注释用语)

> 骨骼动画纹理(SkeletonManager 所有) = 固定 **2048×2048 RGBA32F** pose atlas:
> 行宽容纳 `floor(2048/(4*bone_count))` 个 pose,每个 pose 宽 `4*bone_count` texel
> (每骨一个 4×4 矩阵 = 4 texel),**整 pose 不跨行**,行尾 padding 不使用。inverse_bind
> 走 **骨架级 SSBO/UBO(constant)** 而非 per-instance;instance 的每个 pose 用 CPU 端
> 预算好的 **(row,col) 行列 offset** 作 per-instance attribute 传入,VS 零除法。
