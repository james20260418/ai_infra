// JPOV skeleton — SkeletonManager（GPU 资源对象）实现
//
// 对应 src/skeleton/skeleton_manager.h 契约（架构文档 §3 方案甲 + docs/jpov_skeleton_manager_design.md）：
//   - 构造即绑定一种骨架（SkeletonType）+ 一整套 pose → 烘焙并上传**一张** pose atlas
//     纹理（RGBA32F 固定 kPoseAtlasDim²），CPU 在烘焙期把每 pose 的 jointWorld(相对根)×
//     骨架级 inverseBind 乘好成「最终肤矩阵」落 atlas 行。GPU 无独立逆绑定资源（不用 SSBO，
//     工程 GL 层不可移植——见头文件 方案甲 注释）。
//   - 析构释放 GL 纹理。
//
// 烘焙数学（对照权威 skinning 公式 jointMatrix(j)=globalJoint(j)·inverseBind(j)）：
//   bone j 相对角色原点的世界矩阵沿骨架树自根向下复合：
//     jointLocal(j) = T(rest_offset[j]) · R(pose.joint_rotation[j])   （局部：先平移骨长+再转）
//     child = parent × jointLocal(child)                             （parent 沿 joints 树向上）
//   finalMatrix(j, pose) = jointWorld(j, pose) · inverse_bind(j)      （折入逆绑定，方案甲）
//   pose 之行号/bone 序取 atlas 中该 (pose,bone) 的 4 个 texel（行序）→ 蒙皮 VS 点采样即得。
//
// ⚠️ 语义说明（Prep 阶段）：SkeletonJoint.rest 只建模平移(未含 rest 旋转，见 skeleton_types
//   TODO)；关节的世界位姿驱动 = 固定骨长(rest_offset 平移) + pose 给每骨旋转。最后再乘
//   glTF/资产自带的 inverse_bind 补偿 rest —— 是否在纯 identity pose 下能完美还原绑定网格，
//   属于「真正接 render/gold 验证」的下一个 PR 范畴；本 PR 只保证 CPU 烘焙链路自洽可单测、
//   atlas 上传正确。坐标/双 pose 插值等后续 PR。

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
// primitives3d 的 float[16] + GLSL mat4 约定一致）。此处只留一个本地别名 + atlas 行布局辅助。
using Mat4 = geom::math::Mat4;
using geom::math::JointLocal;
using geom::math::Mat4Identity;
using geom::math::Mat4Mul;

// 把列主序 Mat4 写入 RGBA32F texel 行缓冲：binary 骨占 4 个连续 texel，每个 texel（RGBA=4 float）
//   存矩阵的**一行**（行 r 的分量 = m[col*4+r] for col 0..3）。
// 布局匹配 atlas: 每 (pose) 占 bone_count×4 texel（横排同一 scanline）。
void PutMat4Row(const Mat4& mat, int bone_x0, std::vector<float>* out_row) {
    for (int r = 0; r < 4; ++r) {
        const int texel = bone_x0 + r;  // bone_x0..bone_x0+3 这 4 个 texel 各存矩阵的一行
        float* out = out_row->data();
        out[(texel) * 4 + 0] = mat.m[r];        // col0 row r
        out[(texel) * 4 + 1] = mat.m[4 + r];    // col1 row r
        out[(texel) * 4 + 2] = mat.m[8 + r];    // col2 row r
        out[(texel) * 4 + 3] = mat.m[12 + r];   // col3 row r
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
    // 每 pose 占 bone*4 texel(每骨 4 texel)。pose_per_row = floor(dim/(bone*4))。
    const int pose_tex_w = bone * 4;
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
    // 实际用行数组织 CPU 缓冲(=ceil(pose/pose_per_row) 条全宽 scanline)，再逐行上传——
    // 但 GL 上行总高=2048 固定(纹理尺寸),多出区域填 0 不采样。
    // 简化实现: 分配一个全宽 float 缓冲存"一条 scanline"(2048 texel*4) 逐 Y 构造并上传。

    // 生成 GL 纹理对象。
    glGenTextures(1, &handles_.pose_atlas_tex);
    CHECK_NE(handles_.pose_atlas_tex, 0u) << "SkeletonManager: glGenTextures failed";
    glBindTexture(GL_TEXTURE_2D, handles_.pose_atlas_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, kPoseAtlasDim, kPoseAtlasDim, 0,
                 GL_RGBA, GL_FLOAT, nullptr);

    // 按 scanline(Y) 逐行写入：Y 行容纳 [poseIdx = Y*pose_per_row_, (Y+1)*pose_per_row_.
    // (实际 pose 超多的用很多行; 每行 pose 靠 pose_per_row X 横铺)。行宽 X = kPoseAtlasDim。
    // 注意一行里 pose p(全局序)的 phase 横坐标 = (p % pose_per_row_)*pose_tex_w。
    const int full_rows = (pose_count_ + pose_per_row_ - 1) / pose_per_row_;
    std::vector<float> line(kPoseAtlasDim * 4, 0.0f);  // 全宽 scanline(RGBA)
    for (int y = 0; y < std::min(full_rows, kPoseAtlasDim); ++y) {
        std::fill(line.begin(), line.end(), 0.0f);  // padding 清零
        const int p0 = y * pose_per_row_;
        const int p1 = std::min(pose_count_, p0 + pose_per_row_);
        for (int p = p0; p < p1; ++p) {
            const SkeletonPose& pose = poses[p];
            if (pose.bone_count != 0) {
                CHECK_EQ(pose.bone_count, bone)
                    << "SkeletonManager: pose[" << p << "] bone_count "
                    << pose.bone_count << " 应 == 骨架 " << bone;
            }
            const int x0 = (p % pose_per_row_) * pose_tex_w;  // 本 pose 横向起始 texel
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
                Mat4 final_m = geom::math::Mat4Mul(jw[j], inv[j]);
                const int bone_x0 = x0 + j * 4;
                PutMat4Row(final_m, bone_x0, &line);
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
