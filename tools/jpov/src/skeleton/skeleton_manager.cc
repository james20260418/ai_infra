// JPOV skeleton — SkeletonManager（GPU 资源对象）实现
//
// 对应 src/skeleton/skeleton_manager.h 契约（架构文档 §3 方案甲 + docs/jpov_skeleton_manager_design.md）：
//   - 构造即绑定一种骨架（SkeletonType）+ 一整套 pose → 烘焙并上传**一张** pose atlas
//     纹理（RGBA32F 固定 kPoseAtlasDim²），CPU 在烘焙期把每 pose 的 jointWorld(相对根)×
//     骨架级 inverseBind 乘好，再把得到的**刚体矩阵**转成**对偶四元数**落 atlas 行。
//     GPU 无独立逆绑定资源（不用 SSBO，工程 GL 层不可移植——见头文件 方案甲 注释）。
//
// 烘焙数学（2026-09-18 起为 DQS：**对偶四元数**；矩阵/四元数定义见 geom/math/dual_quat.h）：
//   bone j 相对角色原点的世界矩阵沿骨架树自根向下复合：
//     jointLocal(j) = T(rest_offset[j]) · R(pose.joint_rotation[j])   （局部：先平移骨长+再转）
//     child = parent × jointLocal(child)                             （parent 沿 joints 树向上）
//   finalMatrix(j, pose) = jointWorld(j, pose) · inverse_bind(j)      （折入逆绑定，方案甲）
//   → 刚体矩阵（本链路只含旋转/平移）⇒ 无损转成对偶四元数 q̂ = q + ε·t，t = ½·v̂ ⊗ q：
//     atlas 每骨 2 texel：texel0 = 实部 q(xyzw)、texel1 = 对偶部 t(xyzw)；
//     蒙皮 VS 逐骨取 q、t，做**刚体混合**（DLB）后直接变换顶点（见 skinning_shader.h）。
//
//   【保留：改动前的 LBS（线性混合蒙皮）公式，作为对照/历史】
//     蒙皮链 = Σ_i weight_i · finalMatrix(j_i) · rest_pos（矩阵加权平均），
//     法线 = Σ_i weight_i · mat3(finalMatrix(j_i)) · rest_normal。
//     矩阵加权平均一般**不是**旋转（正交性被破坏）⇒ 关节弯折处顶点被“拉向弦”，
//     体积塌陷/扭转糖纸。DQS 正是为消除该伪影而换的表示（见 dual_quat.h 文件头）。
//
// ⚠️ 语义说明（历史，仍成立）：SkeletonJoint.rest 只建模平移；关节的世界位姿驱动 =
//   固定骨长(rest_offset 平移) + pose 给每骨旋转。最后再乘 glTF/资产自带的 inverse_bind
//   补偿 rest。

// geom/math_util.h(经 skeleton_types→quaternion→vec)使用 M_PI/M_PI_2，须在首次 include
// <cmath> 前定义 _USE_MATH_DEFINES，否则 MinGW 下未定义(生效太晚)。与 render_command.h 同款保护。
#define _USE_MATH_DEFINES
#include <cmath>

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/skeleton/skeleton_manager.h"

// GL 头须在 MinGW 宏替换 #define 之前(常量声明优先)。次序与 renderer.cc / texture_manager.cc 一致：
// GL 常量声明 → 再 gl_loader 宏别名。glext.h 提供 GL_RGBA32F 等(renderer.cc 同引)。
#include <GL/gl.h>
#include <GL/glext.h>

#include <cstring>
#include <vector>

#include <glog/logging.h>
#include "geom/math/dual_quat.h"
#include "geom/math/mat4.h"
#ifdef _WIN32
#include "third_party/gl_loader-mingw/gl_loader.h"
// MinGW 的 GL/gl.h / gl_loader.h 可能不定义 GL_CLAMP_TO_EDGE(renderer.cc 兜底同款)。
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#endif

namespace jpov {

namespace {

// 烘焙用的 4x4 矩阵与基础运算统一来自 geom/math/mat4.h（列主序 float[16]，与 object3d /
// primitives3d 的 float[16] + GLSL mat4 约定一致）；刚体矩阵 → 对偶四元数用 geom/math/dual_quat.h。
// 此处只留本地别名 + atlas 行布局辅助。
using Mat4 = geom::math::Mat4;
using geom::math::DualQuat;
using geom::math::JointLocal;
using geom::math::Mat4Mul;

// 把一根骨的**对偶四元数**写进 atlas 行缓冲：一根骨占 **2 个连续 texel**
//   texel0 = 实部 q(x,y,z,w)     ← 旋转
//   texel1 = 对偶部 t(x,y,z,w)   ← ½·v̂⊗q（平移编码在其中，见 dual_quat.h）
// 每个 texel 各自按**全局平坦下标**判自己落在哪一行（一个 pose/一根骨的 texel 可能跨行，
// 见 ctor 里 “一个 pose 的 texel 可能跨行” 注释）；行内 x = flat − row_lo。
// Pre: bone_flat0 是该骨在本 pose 内的平坦起点；line 为整行缓冲（kPoseAtlasDim×4 float）。
void PutDualQuatTexels(const DualQuat& dq, int bone_flat0, int row_lo, int row_hi,
                       std::vector<float>* line) {
    const geom::Quaternion<float> parts[2] = {dq.q, dq.t};
    for (int k = 0; k < 2; ++k) {
        const int flat = bone_flat0 + k;
        if (flat < row_lo || flat >= row_hi) {
            continue;  // 本 texel 不在此行
        }
        float* out = line->data();
        const int texel = flat - row_lo;  // 本行内的 x
        out[texel * 4 + 0] = parts[k].x;
        out[texel * 4 + 1] = parts[k].y;
        out[texel * 4 + 2] = parts[k].z;
        out[texel * 4 + 3] = parts[k].w;
    }
}

}  // namespace

// ==================== SkeletonManager ====================

SkeletonManager::SkeletonManager(const SkeletonType& type,
                                 std::vector<SkeletonPose> poses) {
    type.Validate();
    const int bone = type.bone_count();
    CHECK_GT(bone, 0) << "SkeletonManager: bone_count 必须 >0";
    const int pose_count = static_cast<int>(poses.size());
    CHECK_GT(pose_count, 0) << "SkeletonManager: 至少一个 pose";

    bone_count_ = bone;
    pose_count_ = pose_count;
    // 每 pose 占 bone*2 texel（每骨 2 texel：实部 q + 对偶部 t）。
    // pose_per_row = floor(dim/(bone*2)) —— 仅作**容量估算**（pose_capacity 的推导）；
    // 运行期取址不依赖它（见 ctor 注释）。
    const int pose_tex_w = bone * 2;
    pose_per_row_ = kPoseAtlasDim / pose_tex_w;
    CHECK_GT(pose_per_row_, 0)
        << "SkeletonManager: 一行放不下一个 pose(bone 过大?) bone=" << bone;
    capacity_ = (kPoseAtlasDim / pose_tex_w) * kPoseAtlasDim;
    CHECK_LE(pose_count_, capacity_)
        << "SkeletonManager: pose " << pose_count_ << " 超 atlas 容量 " << capacity_
        << "(bone=" << bone << ")。换更小骨架或拆多份 SkeletonManager";

    // 骨架级 inverse_bind：**派生量**，由 joints(骨长) + bind_rotation(bind 朝向) 现算
    // （inverse_bind[j] = JW_bind[j]⁻¹；见 SkeletonType::ComputeInverseBind）。
    // 2026-09-11 起 inverse_bind 不再是 SkeletonType 的字段（避免"输入 + 输入的导出物"两份数据打架）。
    // 外部资产若要精确还原其自带绑定关系，应在构造 SkeletonType 时把资产 IBM 共轭到本骨架
    // 坐标系后再进来（见 docs/jpov_retarget_design.md §5.5）；此处只认骨架自身的 bind 形态。
    std::vector<Mat4> inv(bone);
    {
        const std::vector<std::array<float, 16>> ibm = type.ComputeInverseBind();
        CHECK_EQ(ibm.size(), static_cast<size_t>(bone))
            << "SkeletonManager: ComputeInverseBind 尺寸异常";
        for (int j = 0; j < bone; ++j) {
            for (int k = 0; k < 16; ++k) {
                inv[j].m[k] = ibm[static_cast<size_t>(j)][k];
            }
        }
    }

    // ---- 1. 预解骨架树的每骨局部 rest 位移矩阵(与关节 index 对应) ----
    // jointLocalRest 不依赖 pose；但 local 旋转在每个 pose 变。我们逐 pose 现算 local 旋转。
    // 复用 joints tree: 每骨 rest_offset 已知。

    // ---- 2. 逐 pose 烘焙成 atlas 行缓冲(一整张 kPoseAtlasDim² 行优先填) ----
    // GL_RGBA32F 纹理,每 texel 4 float。行缓冲宽度=kPoseAtlasDim texels;每行填满后当下一条。
    // 用"全尺寸一次性 glTexImage2D + 转行缓冲"会爆 memory(2048²*4*4B=64MB×1)；这里按
    // 实际用行数组织 CPU 缓冲(=ceil(总 texel/宽) 条全宽 scanline)，再逐行上传——
    // 但 GL 上行总高=2048 固定(纹理尺寸),多出区域填 0 不采样。
    // 简化实现: 分配一个全宽 float 缓冲存"一条 scanline"(2048 texel*4) 逐 Y 构造并上传。

    // 生成 GL 纹理对象。
    glGenTextures(1, &handles_.pose_atlas_tex);
    CHECK_NE(handles_.pose_atlas_tex, 0u) << "SkeletonManager: glGenTextures failed";
    glBindTexture(GL_TEXTURE_2D, handles_.pose_atlas_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, kPoseAtlasDim, kPoseAtlasDim, 0,
                 GL_RGBA, GL_FLOAT, nullptr);

    // 按**行优先**平铺写入：把每个 pose 的 bone*2 个 texel 沿 X 横铺，铺满一行
    // (kPoseAtlasDim texel) 后换下一行。等价于：atlas 是一段连续 texel，pose 全局序 p 的
    // 起始平坦下标 flat = p * pose_tex_w，其纹理坐标 = (flat % W, flat / W)。
    //
    // ⚠️ 关键：**一个 pose 的 texel 可能跨行**（23 骨、W=2048 时 flat=2024 的 pose 就跨
    //   第 0/1 行）。因此不能「按 scanline 分组、每组内假设 pose 完整落在本行」地写
    //   ——那样跨行 pose 的 x0 会算成负数/越界（本 PR 修掉的崩溃）。
    //   正确做法：**逐 pose 逐 texel**，每个 texel 各自算它的 (x, y) 落到哪一行。
    //
    // 上传策略：仍逐 scanline（每行一次 glTexSubImage2D），用一个全宽行缓冲；
    //   先按行组织好所有 texel 再逐行上传。行缓冲只需一行（2048*4 float = 32KB）。
    const int full_flat = pose_count_ * pose_tex_w;
    const int full_rows = (full_flat + kPoseAtlasDim - 1) / kPoseAtlasDim;
    CHECK_LE(full_rows, kPoseAtlasDim)
        << "SkeletonManager: 需要的行数 " << full_rows << " 超过 atlas 高度 "
        << kPoseAtlasDim << "（pose_count=" << pose_count_ << " bone=" << bone << "）";

    // 行缓冲：W*4 float（RGBA32F 一行）。逐行：清零 → 填本行覆盖的 texel → 上传。
    std::vector<float> line(static_cast<size_t>(kPoseAtlasDim) * 4, 0.0f);
    for (int y = 0; y < full_rows; ++y) {
        std::fill(line.begin(), line.end(), 0.0f);  // padding 清零
        // 本行覆盖的平坦区间 [y*W, (y+1)*W)。哪些 pose 与之相交：从
        //   p_begin = (y*W) / pose_tex_w 起，直到其起点 >= (y+1)*W。
        const int row_lo = y * kPoseAtlasDim;
        const int row_hi = row_lo + kPoseAtlasDim;  // 半开区间
        int p = row_lo / pose_tex_w;
        for (; p < pose_count_ && p * pose_tex_w < row_hi; ++p) {
            const SkeletonPose& pose = poses[p];
            if (pose.bone_count != 0) {
                CHECK_EQ(pose.bone_count, bone)
                    << "SkeletonManager: pose[" << p << "] bone_count "
                    << pose.bone_count << " 应 == 骨架 " << bone;
            }
            // 骨的世界矩阵：局部 = T(rest 平移) × R(bind 朝向) × R(pose 旋转)，沿树复合。
            //   jointWorld[j] = (j==根? I4 : jointWorld[parent]) · local(j)
            // bind_rotation 为空时按恒等处理（兼容"骨长即朝向"的极简骨架）。
            std::vector<Mat4> jw(bone);
            for (int j = 0; j < bone; ++j) {
                const Vec3f& off = type.joints[j].rest_offset;
                const geom::Quaternion<float> bind =
                    type.bind_rotation.empty()
                        ? geom::Quaternion<float>::Identity()
                        : type.bind_rotation[j];
                const geom::Quaternion<float> pose_rot =
                    pose.joint_rotation.size() > static_cast<size_t>(j)
                        ? pose.joint_rotation[j]
                        : geom::Quaternion<float>::Identity();
                Mat4 local = geom::math::JointLocal(off, bind, pose_rot);
                if (type.joints[j].parent == kSkeletonNoParent) {
                    jw[j] = local;
                } else {
                    const int pr = type.joints[j].parent;
                    CHECK(pr >= 0 && pr < j) << "parent 拓扑序乱(应在 Validate 抓)";
                    jw[j] = geom::math::Mat4Mul(jw[pr], local);
                }
                // final = jointWorld[j] × inverse_bind[j] (方案甲折入)
                const Mat4 final_m = geom::math::Mat4Mul(jw[j], inv[j]);
                // 刚体矩阵 → 对偶四元数（DQS 的数据表示；数学见 geom/math/dual_quat.h）。
                // 含缩放/剪切的非刚体矩阵会在此 LOG(FATAL)（本链路恒刚体，见文件头）。
                const DualQuat dq = geom::math::DualQuatFromRigidMatrix(final_m);
                // 本骨在本 pose 内的平坦下标 = p*pose_tex_w + j*2（2 texel：q 与 t）；
                // 2 个 texel 逐个落到 (x=flat%W, y=flat/W) —— **逐 texel 判行**，跨行自然处理。
                const int bone_flat0 = p * pose_tex_w + j * 2;
                PutDualQuatTexels(dq, bone_flat0, row_lo, row_hi, &line);
            }
        }
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, kPoseAtlasDim, 1, GL_RGBA,
                        GL_FLOAT, line.data());
    }
    // 若用行 < 高度，余行未上传但纹理已建(全 0) → 不影响已引用 pose(行号都在 used 内)。
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);  // 点采样，不插值矩阵
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    handles_.bone_count = bone;
    handles_.pose_per_row = pose_per_row_;

    GLenum err = glGetError();
    CHECK_EQ(err, GL_NO_ERROR)
        << "SkeletonManager: GL error after bake, code=" << err;

    LOG(INFO) << "SkeletonManager ctor ok: bone=" << bone << " pose=" << pose_count_
              << " pose_per_row=" << pose_per_row_ << " capacity~" << capacity_;
}

SkeletonManager::~SkeletonManager() {
    if (handles_.pose_atlas_tex != 0) {
        glDeleteTextures(1, &handles_.pose_atlas_tex);
        handles_.pose_atlas_tex = 0;
    }
}

}  // namespace jpov
