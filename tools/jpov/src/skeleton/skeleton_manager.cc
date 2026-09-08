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

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/skeleton/skeleton_manager.h"

// GL 头须在 MinGW 宏替换 #define 之前(常量声明优先)。次序与 renderer.cc / texture_manager.cc 一致：
// GL 常量声明 → 再 gl_loader 宏别名。
#include <GL/gl.h>

#include <cstring>
#include <vector>

#include <glog/logging.h>
#ifdef _WIN32
#include "third_party/gl_loader-mingw/gl_loader.h"
#endif

namespace jpov {

namespace {

// ==================== 极简列主序 mat4 ====================
// float[16]，**列主序**（与 object3d / primitives3d 的 float[16] + GLSL mat4 约定一致）：
//   m[col*4+row]。用于烘焙时把 jointLocal/world 复合、乘 inverse_bind。
// 静止态矩阵缩放/translation 均在此自校验，不引入其它 mat 库。内部列主序；
// 落 atlas 时转成**行序** 4×vec4（每 texel 一个矩阵行，见 PutMat4ToRow）。

struct Mat4 {
    float m[16];
};

Mat4 IdentityMat4() {
    Mat4 r;
    std::memset(r.m, 0, sizeof(r.m));
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

// 平移矩阵（列主序）。
Mat4 TranslationMat4(float x, float y, float z) {
    Mat4 r = IdentityMat4();
    r.m[12] = x;  // col3 row0
    r.m[13] = y;  // col3 row1
    r.m[14] = z;  // col3 row2
    return r;
}

// 由单位四元数 q 构造旋转矩阵（列主序）。pre: q 已归一（调用方保证，或先归一）。
Mat4 RotationMat4(const geom::Quaternion<float>& q) {
    geom::Quaternion<float> n = q;
    n.NormalizeInPlace();
    const float x = n.x, y = n.y, z = n.z, w = n.w;
    Mat4 r = IdentityMat4();
    // 标准 row-major m00.. 再转列主序；直接按列写最省事。
    // col0
    r.m[0] = 1 - 2 * (y * y + z * z);
    r.m[1] = 2 * (x * y + z * w);
    r.m[2] = 2 * (x * z - y * w);
    // col1
    r.m[4] = 2 * (x * y - z * w);
    r.m[5] = 1 - 2 * (x * x + z * z);
    r.m[6] = 2 * (y * z + x * w);
    // col2
    r.m[8]  = 2 * (x * z + y * w);
    r.m[9]  = 2 * (y * z - x * w);
    r.m[10] = 1 - 2 * (x * x + y * y);
    return r;
}

// 矩阵乘法 c = a × b（列主序）。矩阵按列主序，c = a*b 即「先 b 再 a」对列向量。
Mat4 MulMat4(const Mat4& a, const Mat4& b) {
    Mat4 c;
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a.m[k * 4 + row] * b.m[col * 4 + k];
            }
            c.m[col * 4 + row] = sum;
        }
    }
    return c;
}

// 把列主序 Mat4 写入 RGBA32F texel 行缓冲：第 j 行（矩阵行）一个 vec4（RGBA）。
//   out_row 为宽度 >= (bone*4) 的 float 缓冲（每 texel 4 float)。写入坐标：
//   base_texel = x0 + bone_idx*4 + row(0..3)，每个 texel 是矩阵一行的 4 分量。
// 布局匹配 atlas: 每 (pose) 占 bone_count×4 texel（横排同一 scanline），每骨 4 texel = 4 行。
void PutMat4Row(const Mat4& mat, int bone_x0, std::vector<float>* out_row) {
    // mat 列主序;texel 存矩阵**行**。行 r 的分量 = m[col*4 + r] for col 0..3。
    for (int r = 0; r < 4; ++r) {
        const int texel = bone_x0 + r;  // 每 bone 用 4 个连续 texel，texel t 存矩阵第 t 行? no——
        // 上面注释：bone 占 4 texel，每 texel 是矩阵**一行**（一个 vec4 4 分量）。
        // 因一个 texel(RGBA) = 4 float = 恰矩阵一行;所以 bone 需 4 个 texel 存 4 行,
        // 即 bone_x0..bone_x0+3 这 4 个 texel 各存矩阵的一行。
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

    // 骨架级 inverse_bind（可选；缺省=单位阵，烘焙时以单位代替）。
    std::vector<Mat4> inv(bone);
    const bool has_ibm = type.inverse_bind.size() == static_cast<size_t>(bone);
    for (int j = 0; j < bone; ++j) {
        if (has_ibm) {
            // SkeletonType.inverse_bind 存的是 float[16]，列主序（ctor 注释约定）。原样搬。
            for (int k = 0; k < 16; ++k) {
                inv[j].m[k] = type.inverse_bind[j][k];
            }
        } else {
            inv[j] = IdentityMat4();
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
            // 骨的世界矩阵: 用局部(rest 平移 × pose 旋转) 沿树复合
            // jointWorld[j] = (j==根? I4 : jointWorld[parent]) · T(rest[j]) · R(rot[j])
            std::vector<Mat4> jw(bone);
            for (int j = 0; j < bone; ++j) {
                const Vec3f& off = type.joints[j].rest_offset;
                Mat4 local = MulMat4(TranslationMat4(off.x(), off.y(), off.z()),
                                     (pose.joint_rotation.size() > static_cast<size_t>(j)
                                          ? RotationMat4(pose.joint_rotation[j])
                                          : IdentityMat4()));
                if (type.joints[j].parent == kSkeletonNoParent) {
                    jw[j] = local;
                } else {
                    const int pr = type.joints[j].parent;
                    CHECK(pr >= 0 && pr < j) << "parent 拓扑序乱(应在 Validate 抓)";
                    jw[j] = MulMat4(jw[pr], local);
                }
                // final = jointWorld[j] × inverse_bind[j] (方案甲折入)
                Mat4 final_m = MulMat4(jw[j], inv[j]);
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
