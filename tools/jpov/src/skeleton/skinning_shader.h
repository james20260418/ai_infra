// JPOV skeleton skinning — 蒙皮顶点/片元着色器 + 渲染入口
//
// 目的：把「rest mesh 顶点 (带 JOINTS_0/WEIGHTS_0) 经 SkeletonManager 的 pose atlas
// 蒙皮，再走与 object3d PBR 相同的光照」落成可渲染路径。
//
// ═══════════════ 蒙皮算法：DQS（对偶四元数）—— 2026-09-18 起 ═══════════════
// 本 pass 用 **Dual Quaternion Skinning**：atlas 每根骨存一个**对偶四元数**
//   q̂ = q + ε·t（实部 q = 旋转，对偶部 t = ½·v̂⊗q 编码平移；数学/推导/单测见
//   geom/math/dual_quat.h + geom/math/dual_quat_test.cc），顶点变换为
//     v' = DLB( {w_i, q̂_i} ) 作用在 rest 顶点上
//   其中 DLB（Dual Quaternion Linear Blending，Kavan 2007）=
//     ① 逐骨两帧插值：NLERP(q̂_a, q̂_b, ratio)（四元数空间插值 + 归一化）
//     ② 抗对偶：把参与混合的 q̂ 与**参考骨**（权重最大那根）统一到同一半球
//     ③ 加权求和 Σ w_i·q̂_i，再按 |Σ w_i·q_i| **同除实部与对偶部**归一化
//     ④ 用该 q̂ 变换顶点（旋转 + 平移）；法线/切线只用其旋转部分
//   ⇒ 混合结果**仍是刚体变换**，不塌体积、不糖纸（LBS 的核心缺陷）。
//
// 【保留：改动前的 LBS 实现（线性混合蒙皮），仅作对照/历史，不再执行】
//   atlas 当时每骨存 4×4「最终肤矩阵」（行主序 4 texel），VS 侧：
//     mat4 m = LoadBoneMatrixAt(pose_col, bone);          // 4 texel 还原 mat4
//     sp += w * (m * vec4(aPos, 1.0)).xyz;                // 位置：Σ w·M·v
//     sn += w * vec3(mat3(m) * aNormal);                  // 法线：Σ w·mat3(M)·n
//     st += w * vec3(mat3(m) * aTangent);                 // 切线：同上
//     // 两帧之间：在**矩阵空间**逐列 mix（mix(ma[k], mb[k], ratio)）后蒙皮
//   问题：Σ w_i·M_i 一般不是旋转（正交性被破坏）⇒ 关节弯折处顶点被“拉向弦”而体积塌陷，
//   扭转时出糖纸。DQS 换掉的正是这一步（矩阵加权平均 → 刚体变换混合）。
//
// 设计（M1 起沿用，未变）：
//   - VS 与 object3d PBR 完整版共用同一套 uniform( uViewProj / 摆放矩阵 )与输出 varying：
//     vWorldPos / vWorldNormal / vTexCoord / vWorldTangent —— 因此 **FS 直接复用
//     kMeshFs3dPBR（同光照），不重写片元**。
//   - 顶点蒙皮发生在 mesh 局部空间，蒙皮后再做本实例摆放矩阵 aInstModel(center/up/front/
//     scale)：世界法线/切线沿用 object3d 的 mat3(transpose(inverse(uModel))) 约定 —— 这样在
//     bind pose 下（每骨 q̂ = 单位元）本 VS 的输出与 object3d PBR 完整版**逐位一致**，
//     保证静态 gold 零回归（dual_quat_test 的 BlendOfIdentityBonesIsExactIdentity 是这条的
//     数学依据）。
//   - 每骨 2 个 texel（q + t）在 atlas 为**行优先平铺**：flat → (flat % W, flat / W)。
//     一个 pose 的 texel **可能跨行**（23 骨 = 46 texel 时必然跨），故逐 texel 各自回绕，
//     与 CPU 烘焙（skeleton_manager.cc PutDualQuatTexels）逐 texel 对齐。
//
// ⚠️ 本文件里的两份 VS（主 pass / 阴影 pass）**必须逐字同公式** —— GLSL 没有 #include，
//   两个 program 各持一份完整源码；PR #104 的教训：主/阴影蒙皮公式一旦分叉，影子就和身体
//   错位。改其中一处时必须同步改另一处。

#ifndef JPOV_SRC_SKELETON_SKINNING_SHADER_H_
#define JPOV_SRC_SKELETON_SKINNING_SHADER_H_

namespace jpov {

// ==================== per-instance attribute 布局（主 pass / shadow pass 共用） ====================
//
// 「真 instanced draw」的关键：实例间的差异必须走 **per-instance attribute**（divisor=1），
// 不能走逐实例 uniform —— 后者必须逐实例一次 draw（N 实例 = N 次 draw call）。
// 属性槽：loc6..9 = aInstModel，loc10 = aInstPose。
//   布局的**唯一约定点**在 src/instance_buffer.h（kInstanceModelAttrSpec / kInstancePoseAttrSpec）；
//   实例数据由渲染器持有的 InstanceBuffer 承载（属于「这次 draw」，不属于 mesh）。
//
//   摆放： location 6..9 = aInstModel (mat4，4 个 vec4 slot)
//          ★ 全批共享 VAO 的**顶点**属性（loc0-5）不变，与普通 draw 完全一致。
//
// ⚠️ **视图/投影矩阵不入 per-instance attribute**：它全批共享、每帧一张，走 uniform
//   （uViewProj / uShadowViewProj / uShadowDepthViewProj），满足 minimal surprise ——
//   把恒定值放 attribute 会浪费带宽、也让调用方以为它可逐实例变。
//   着色器里世界变换 = uViewProj * aInstModel * (骨架空间顶点)。这是标准 instancing 分体：
//   **per-instance = 摆放，per-frame = 相机。**
//
// 布局与 shader 侧 layout(location=N) 声明一一对应：
//   loc6 = 第 0 列(vec4)  loc7 = 第 1 列  loc8 = 第 2 列  loc9 = 第 3 列
//   （列主序，与 BuildModelMatrix 输出一致，见 instance_buffer.h 的布局表）。

// ==================== 蒙皮顶点着色器（DQS） ====================
// 输出与 object3d kMeshVs3dPBRFull 完全一致（vWorldPos/vWorldNormal/vTexCoord/vWorldTangent），
// 仅把「顶点经 uModel」改为「顶点先经 atlas 对偶四元数蒙皮、再经 uModel」。
// 因此 FS 端直接复用 kMeshFs3dPBR（同光照），保证 sunny-day 效果与 object3d 一致。
inline constexpr const char* kSkinnedVs = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in ivec4 aJoint;
layout(location = 4) in vec4 aWeight;
layout(location = 5) in vec3 aTangent;

uniform mat4 uViewProj;   // proj*view（每帧一张，全批共享）；世界→裁剪。model 走 aInstModel。
// per-instance 摆放矩阵（location 6..9 拆成 4 列；divisor=1，每实例推进一步）。
layout(location = 6) in vec4 aInstCol0;
layout(location = 7) in vec4 aInstCol1;
layout(location = 8) in vec4 aInstCol2;
layout(location = 9) in vec4 aInstCol3;
// per-instance pose 选择（divisor=1）：vec3(pose_col_a, pose_col_b, ratio)。
//   .x/.y = 本实例 pose_a / pose_b 在 atlas 里的**平坦 texel 起点**（= pose_idx * bone_count * 2，
//           CPU 端算好；每骨占 2 texel）
//   .z    = pose_a→pose_b 插值权重 [0,1]（静态时为 0，短路不读 pose_b）
//   与 aInstModel 同理：逐实例差异必须走 attribute，不然整批就退化回 N 次 draw。
//   ⚠️ 统一 float（而非 ivec2 + IPointer）：IPointer 按**原始整数位**解释，float 的位
//   会被读成天文数字列 → texelFetch 出界、整批塌成空白（踩过）。
layout(location = 10) in vec3 aInstPose;
uniform sampler2D uPoseAtlas;  // RGBA32F 骨骼动画纹理（pose atlas，每骨 2 texel：实部 q + 对偶部 t）
uniform int   uBoneCount;      // 该骨架骨数
uniform int   uPoseRow;        // 本实例 pose 在 atlas 的行（y）
uniform vec2  uAtlasDim;       // atlas 纹理尺寸 (w,h)，平坦→(x,y) 回绕用

out vec3 vWorldPos;
out vec3 vWorldNormal;
out vec2 vTexCoord;
out vec3 vWorldTangent;

// 从 atlas 取第 bone 根骨的**对偶四元数** q̂ = q + ε·t（每骨 2 个连续 texel）。
//   texel0 = 实部 q(xyzw)（旋转），texel1 = 对偶部 t(xyzw)（t = ½·v̂⊗q，平移编码）
// 取址：pose_col 是**本 pose 在 atlas 里的平坦 texel 起点**（CPU 端算好 = pose_idx * bone*2）。
//   **不再乘 uBoneCount**：那样把「每 pose 宽 = bone_count*2」写死进 shader，且与 CPU 的
//   平铺布局假设分叉（历史 bug：两侧对“一行放几个 pose”理解不同 → 取到未上传的黑行）。
//   atlas 是行优先平铺的一整块 texel：flat → (flat % W, flat / W)。
void LoadDualQuatAt(int pose_col, int bone, out vec4 q, out vec4 t) {
    int x0 = pose_col + bone * 2;
    q = texelFetch(uPoseAtlas, ivec2((x0 + 0) % int(uAtlasDim.x), uPoseRow + (x0 + 0) / int(uAtlasDim.x)), 0);
    t = texelFetch(uPoseAtlas, ivec2((x0 + 1) % int(uAtlasDim.x), uPoseRow + (x0 + 1) / int(uAtlasDim.x)), 0);
}

// 本实例这一帧第 bone 根骨的对偶四元数：在两帧（pose_a / pose_b）之间**在四元数空间**插值。
//
// 为什么不是矩阵 lerp（LBS 时代的做法）：矩阵逐元素线性插值得到的中间态不是旋转，
//   中间帧同样会缩体积；且大角度相邻帧还可能“绕远路”。改为对偶四元数 NLERP：
//   逐分量 mix + 归一化，并在符号相反时按**最短路径**翻转（q 与 −q 表示同一旋转，
//   不翻会插到相反方向去）。
//
// ratio <= 0 时**短路**只取 pose_a：静态/单帧场景（既有 gold 全走这条）取址与插值实现前
//   完全一致（零回归）。
void LoadBoneDualQuat(int bone, out vec4 q, out vec4 t) {
    vec4 qa, ta;
    LoadDualQuatAt(int(aInstPose.x), bone, qa, ta);
    if (aInstPose.z <= 0.0) {
        q = qa;
        t = ta;
        return;
    }
    vec4 qb, tb;
    LoadDualQuatAt(int(aInstPose.y), bone, qb, tb);
    // 最短路径：dot < 0 ⇒ q_b 在另一半球，整体取负（(q,t) 同取负 = 同一个刚体变换）。
    if (dot(qa, qb) < 0.0) {
        qb = -qb;
        tb = -tb;
    }
    float r = aInstPose.z;
    vec4 qm = mix(qa, qb, r);
    vec4 tm = mix(ta, tb, r);
    // 归一化（NLERP）：实部与对偶部**同除 |q|**（只除实部会让平移尺度错，见 dual_quat.h）。
    // 病态保护与本工程 CPU 侧 DualQuatNormalized 一致：|q| 近零时不除（原样返回）。
    // 注：翻符号后 dot ≥ 0 ⇒ |mix|² ≥ (1−r)²+r² ≥ 0.5，实际到不了近零。
    float n = length(qm);
    if (n > 1e-8) {
        qm = qm / n;
        tm = tm / n;
    }
    q = qm;
    t = tm;
}

// 对偶四元数变换点（q/t 视为已归一）：
//   p' = p + 2·q.w·(q.xyz × p) + 2·(q.xyz × (q.xyz × p))      ← 旋转（RotateVector 同式）
//        + 2·(q.w·t.xyz − t.w·q.xyz + q.xyz × t.xyz)           ← 平移（= 2·vec(t⊗q*)）
// 该展开式与 geom/math/dual_quat.h 的 DualQuatTransformPoint 等价，后者由
//   dual_quat_test.cc 的 ShaderFormulaMatchesRotationPlusTranslation 逐位钉住 —— 改公式时
//   同步改那个对照函数与这里。
vec3 DualQuatTransformPoint(vec4 q, vec4 t, vec3 p) {
    vec3 rot = p + 2.0 * q.w * cross(q.xyz, p) + 2.0 * cross(q.xyz, cross(q.xyz, p));
    vec3 tra = 2.0 * (q.w * t.xyz - t.w * q.xyz + cross(q.xyz, t.xyz));
    return rot + tra;
}

// 只用**旋转部分**转方向（法线/切线）：平移对方向无影响。
vec3 DualQuatRotateVector(vec4 q, vec3 v) {
    return v + 2.0 * q.w * cross(q.xyz, v) + 2.0 * cross(q.xyz, cross(q.xyz, v));
}

void main() {
    // ── ① 逐骨取「本帧」对偶四元数（两帧插值在 LoadBoneDualQuat 内完成）──
    vec4 qs[4];
    vec4 ts[4];
    float ws[4];
    int ref = 0;          // 参考骨下标（抗对偶的符号基准）= 权重最大者
    float ref_w = 0.0;    // 参考骨的权重（用严格 > 比较 ⇒ 并列时取小下标，确定）
    float wsum = 0.0;
    for (int i = 0; i < 4; ++i) {
        qs[i] = vec4(0.0, 0.0, 0.0, 1.0);
        ts[i] = vec4(0.0);
        ws[i] = 0.0;
        float w = aWeight[i];
        if (w <= 0.0) {
            continue;
        }
        LoadBoneDualQuat(aJoint[i], qs[i], ts[i]);
        ws[i] = w;
        wsum += w;
        if (w > ref_w) {
            ref_w = w;
            ref = i;
        }
    }

    // ── ② 抗对偶 + ③ 加权求和 ──
    // q̂ 与 −q̂ 是同一个刚体变换，但直接相加会互相抵消（得到垃圾）⇒ 逐骨先与参考骨统一半球。
    // 参考取**权重最大的骨**（逐顶点确定，与姿态/时间无关）；并列时取小下标（见上面严格 >）。
    vec4 q_ref = qs[ref];
    vec4 q_sum = vec4(0.0);
    vec4 t_sum = vec4(0.0);
    for (int i = 0; i < 4; ++i) {
        if (ws[i] <= 0.0) {
            continue;
        }
        float s = (dot(qs[i], q_ref) < 0.0) ? -1.0 : 1.0;
        q_sum += s * ws[i] * qs[i];
        t_sum += s * ws[i] * ts[i];
    }

    // ── ④ 归一化并变换顶点 ──
    // 退化情形（顶点没有任何有效权重，或符号统一后仍完全抵消）→ 取**单位元**，即顶点保持
    // rest 不动。这与 CPU 真值实现（test/skeleton/jpov_skeleton_gold_common.h 里 wsum<=0
    // 分支）逐项一致；改动前的 LBS 版本在这里会把顶点塌到原点（静默错），顺手对齐。
    vec4 dq_q = vec4(0.0, 0.0, 0.0, 1.0);
    vec4 dq_t = vec4(0.0);
    float n_sum = length(q_sum);
    if (wsum > 0.0 && n_sum > 1e-8) {
        dq_q = q_sum / n_sum;
        dq_t = t_sum / n_sum;
    }
    vec3 sp = DualQuatTransformPoint(dq_q, dq_t, aPos);
    vec3 sn = DualQuatRotateVector(dq_q, aNormal);
    vec3 st = DualQuatRotateVector(dq_q, aTangent);

    // 本实例摆放矩阵（per-instance attribute，每实例不同；4 列拼回 mat4）。
    mat4 inst_model = mat4(aInstCol0, aInstCol1, aInstCol2, aInstCol3);
    // 世界坐标/法线：inst_model = center/up/front/scale（与 DrawObject3D 同约定，逐实例）。
    vec4 world = inst_model * vec4(sp, 1.0);
    vWorldPos = world.xyz;
    vWorldNormal = normalize(mat3(transpose(inverse(inst_model))) * sn);
    vWorldTangent = normalize(mat3(transpose(inverse(inst_model))) * st);
    vTexCoord = aTexCoord;
    // 裁剪坐标 = (proj*view) * inst_model * 骨架空间顶点 = uViewProj * world。
    gl_Position = uViewProj * world;
}
)glsl";

// FS 复用 object3d PBR 片元（同光照）。本模块不重复定义；绘制时直接以
// {kSkinnedVs, kMeshFs3dPBR} 注册 program（见 object3d_renderer.h 导出）。

// ==================== 蒙皮阴影顶点着色器（DQS） ====================
// 阴影 pass 专用：在网格局部空间蒙皮(同 kSkinnedVs)后, 用光空间 VP 裁剪 + 输出线性深度
// （与 object3d 的 kShadowVs 语义一致：uShadowViewProj=光VP 用于 gl_Position 近远裁剪；
//   uShadowDepthViewProj=DepthVP 输出的 vShadowDepth 为相对主视锥中心的线性深度, w=1）。
// 输入/输出与 kShadowVs 完全对齐（vShadowDepth），故 FS 复用 kShadowFs。
//
// 与主 pass 同构：摆放矩阵走 per-instance attribute（loc6..9），光空间 VP 走 uniform。
// host 侧 uViewProj ← uShadowViewProj。
//
// ⚠️ 蒙皮部分（LoadDualQuatAt / LoadBoneDualQuat / DualQuatTransformPoint）与 kSkinnedVs
//   **逐字同公式**：主/阴影两 pass 一旦分叉，影子与身体就会错位（PR #104 的教训）。
//   阴影只需位置（不需要法线/切线）。
inline constexpr const char* kSkinnedShadowVs = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in ivec4 aJoint;
layout(location = 4) in vec4 aWeight;
layout(location = 5) in vec3 aTangent;
// per-instance 摆放矩阵（同主 pass：location 6..9 拆 4 列，divisor=1）。
layout(location = 6) in vec4 aInstCol0;
layout(location = 7) in vec4 aInstCol1;
layout(location = 8) in vec4 aInstCol2;
layout(location = 9) in vec4 aInstCol3;
// per-instance pose 选择（divisor=1，同主 pass）：vec3(pose_col_a, pose_col_b, ratio)。
layout(location = 10) in vec3 aInstPose;
uniform mat4 uShadowViewProj;       // 光空间 裁剪（proj*view，model 走 aInstModel）
uniform mat4 uShadowDepthViewProj;  // 光空间 线性深度（DepthProj*view，model 走 aInstModel）
uniform sampler2D uPoseAtlas;
uniform int   uBoneCount;
uniform int   uPoseRow;
uniform vec2  uAtlasDim;       // atlas 纹理尺寸 (w,h)，平坦→(x,y) 回绕用
out float vShadowDepth;

// 同主 pass：每骨 2 texel（实部 q + 对偶部 t）。
void LoadDualQuatAt(int pose_col, int bone, out vec4 q, out vec4 t) {
    int x0 = pose_col + bone * 2;
    q = texelFetch(uPoseAtlas, ivec2((x0 + 0) % int(uAtlasDim.x), uPoseRow + (x0 + 0) / int(uAtlasDim.x)), 0);
    t = texelFetch(uPoseAtlas, ivec2((x0 + 1) % int(uAtlasDim.x), uPoseRow + (x0 + 1) / int(uAtlasDim.x)), 0);
}

// 同主 pass：两帧 NLERP（最短路径 + 归一化），ratio<=0 短路取 pose_a。
void LoadBoneDualQuat(int bone, out vec4 q, out vec4 t) {
    vec4 qa, ta;
    LoadDualQuatAt(int(aInstPose.x), bone, qa, ta);
    if (aInstPose.z <= 0.0) {
        q = qa;
        t = ta;
        return;
    }
    vec4 qb, tb;
    LoadDualQuatAt(int(aInstPose.y), bone, qb, tb);
    if (dot(qa, qb) < 0.0) {
        qb = -qb;
        tb = -tb;
    }
    float r = aInstPose.z;
    vec4 qm = mix(qa, qb, r);
    vec4 tm = mix(ta, tb, r);
    float n = length(qm);
    if (n > 1e-8) {
        qm = qm / n;
        tm = tm / n;
    }
    q = qm;
    t = tm;
}

// 同主 pass：对偶四元数变换点。
vec3 DualQuatTransformPoint(vec4 q, vec4 t, vec3 p) {
    vec3 rot = p + 2.0 * q.w * cross(q.xyz, p) + 2.0 * cross(q.xyz, cross(q.xyz, p));
    vec3 tra = 2.0 * (q.w * t.xyz - t.w * q.xyz + cross(q.xyz, t.xyz));
    return rot + tra;
}

void main() {
    // 蒙皮：与主 pass **完全一致**的流程（参考骨 = 权重最大者；抗对偶；加权求和；归一化）。
    vec4 qs[4];
    vec4 ts[4];
    float ws[4];
    int ref = 0;
    float ref_w = 0.0;
    float wsum = 0.0;
    for (int i = 0; i < 4; ++i) {
        qs[i] = vec4(0.0, 0.0, 0.0, 1.0);
        ts[i] = vec4(0.0);
        ws[i] = 0.0;
        float w = aWeight[i];
        if (w <= 0.0) {
            continue;
        }
        LoadBoneDualQuat(aJoint[i], qs[i], ts[i]);
        ws[i] = w;
        wsum += w;
        if (w > ref_w) {
            ref_w = w;
            ref = i;
        }
    }
    vec4 q_ref = qs[ref];
    vec4 q_sum = vec4(0.0);
    vec4 t_sum = vec4(0.0);
    for (int i = 0; i < 4; ++i) {
        if (ws[i] <= 0.0) {
            continue;
        }
        float s = (dot(qs[i], q_ref) < 0.0) ? -1.0 : 1.0;
        q_sum += s * ws[i] * qs[i];
        t_sum += s * ws[i] * ts[i];
    }
    vec4 dq_q = vec4(0.0, 0.0, 0.0, 1.0);
    vec4 dq_t = vec4(0.0);
    float n_sum = length(q_sum);
    if (wsum > 0.0 && n_sum > 1e-8) {
        dq_q = q_sum / n_sum;
        dq_t = t_sum / n_sum;
    }
    vec3 sp = DualQuatTransformPoint(dq_q, dq_t, aPos);

    // 本实例摆放矩阵（per-instance attribute，同主 pass）。
    mat4 inst_model = mat4(aInstCol0, aInstCol1, aInstCol2, aInstCol3);
    // 光空间裁剪：先经本实例摆放，再乘光空间 VP（model 不再入 VP，与主 pass 对称）。
    vec4 wp = inst_model * vec4(sp, 1.0);
    gl_Position = uShadowViewProj * wp;
    vec4 dpos = uShadowDepthViewProj * wp;
    vShadowDepth = dpos.z / dpos.w;
}
)glsl";

}  // namespace jpov

#endif  // JPOV_SRC_SKELETON_SKINNING_SHADER_H_
