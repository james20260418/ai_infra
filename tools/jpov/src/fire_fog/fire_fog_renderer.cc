// JPOV FireFogRenderer 成员函数实现（froxel 版）
//
// 见 fire_fog_renderer.h / fire_fog_shader.h / tools/jpov/docs/jpov_froxel_design.md。
//
// 趟：inject → reduce×num_levels（金字塔）→ scatter → composite（就地混合进调用方 HDR FBO）。
//
// ⚠️ GL 状态机契约见 fire_fog_renderer.h 顶部。本文件自行保存/复原 FBO 绑定 / viewport /
//    活动纹理单元 / 使用的纹理单元绑定 / blend func；其余 enable 位由调用方 glPushAttrib 兜底。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/fire_fog/fire_fog_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// GL 头文件必须最先 include（在 MinGW #define 宏替换之前）。
// ⚠️ MinGW 也要显式包含 <GL/glext.h>：本文件用到 FBO / RGBA32F / R32F 等 GL3 枚举，
//   它们不在 <GL/gl.h>（仅 GL 1.1），gl_loader.h 也不提供 ⇒ 必须走 glext.h（同 renderer.cc）。
#include <GL/gl.h>
#include <GL/glext.h>

#ifdef _WIN32
// MinGW: windows.h 定义 ERROR 宏与 glog 冲突，必须在 glog 之前 suppress
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
#include "third_party/gl_loader-mingw/gl_loader.h"
// MinGW 的 GL/gl.h 可能不定义 GL_CLAMP_TO_EDGE
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#endif

// Windows/MinGW: windef.h 定义 near/far 宏，与 Camera::near/far 字段冲突。
#ifdef _WIN32
#undef near
#undef far
#endif

#include <glog/logging.h>

#include "tools/jpov/src/fire_fog/fire_fog_lower.h"
#include "tools/jpov/src/fire_fog/fire_fog_shader.h"

namespace {

constexpr int kBodyTexelsPerFog = jpov::fog_layout::kTexelsPerBody;   // 每团 texel 数（见 fog_body.h 布局契约）
// inject 的纹理单元：0=团 tile 索引,1=tile z 范围,2=团属性,3=tile 中心射线,4=z 切片边界,
// 5=点光源 tile 索引；CSM 阴影从 6 起（6..6+kMaxCascades-1）。
constexpr int kLightIndexTexUnit = 5;
constexpr int kShadowTexUnitBase = 6;
// 共享 3D 火纹理单元（sampler2DArray uFireVolTex）。取阴影级联之后、不与任何现有单元冲突。
constexpr int kFireVolTexUnit = kShadowTexUnitBase + jpov::ShadowConfig::kMaxCascades;   // = 11
// 共享火纹理 2D 数组的图层数上限（layers = nz*nframes；GL 常见上限 2048，llvmpipe 亦然）。
constexpr int kMaxFireVolLayers = 2048;

// Draw() 会触碰的纹理单元数：inject 用 0..5（团 tile / z 范围 / 团属性 / tile 射线 /
// z 切片边界 / 光源 tile）+ CSM 6..(6+kMaxCascades-1) + 共享 3D 火纹理（kFireVolTexUnit）；
// scatter 用 0..kMaxLevels；composite 用 0/1。
int TexUnitsTouched() {
    return kFireVolTexUnit + 1;   // 覆盖 0..kFireVolTexUnit
}

// 4x4 列主序求逆（Gauss-Jordan + 部分主元）。无法求逆返回 false。
//（与 horizon_fog_renderer.cc / 点雾版 fire_fog_renderer.cc 同款实现。）
bool Mat4Invert(const float m[16], float out[16]) {
    float a[16];
    for (int i = 0; i < 16; ++i) {
        a[i] = m[i];
    }
    float inv[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    for (int col = 0; col < 4; ++col) {
        int piv = col;
        float best = std::fabs(a[col * 4 + col]);
        for (int r = col + 1; r < 4; ++r) {
            const float v = std::fabs(a[col * 4 + r]);
            if (v > best) {
                best = v;
                piv = r;
            }
        }
        if (best < 1e-12f) {
            return false;
        }
        if (piv != col) {
            for (int c = 0; c < 4; ++c) {
                float t = a[c * 4 + col];
                a[c * 4 + col] = a[c * 4 + piv];
                a[c * 4 + piv] = t;
                t = inv[c * 4 + col];
                inv[c * 4 + col] = inv[c * 4 + piv];
                inv[c * 4 + piv] = t;
            }
        }
        const float d = a[col * 4 + col];
        for (int c = 0; c < 4; ++c) {
            a[c * 4 + col] /= d;
            inv[c * 4 + col] /= d;
        }
        for (int r = 0; r < 4; ++r) {
            if (r == col) {
                continue;
            }
            const float f = a[col * 4 + r];
            for (int c = 0; c < 4; ++c) {
                a[c * 4 + r] -= f * a[c * 4 + col];
                inv[c * 4 + r] -= f * inv[c * 4 + col];
            }
        }
    }
    for (int i = 0; i < 16; ++i) {
        out[i] = inv[i];
    }
    return true;
}

// 组装一趟的 FS 源码（#version 330 core + 公共前置 + 趟体）。
std::string AssembleFragmentShader(const char* body) {
    std::string s = "#version 330 core\n";
    s += jpov::kFireFogCommonGlsl;
    s += body;
    return s;
}

// 建一个 RGBA32F 颜色附件 FBO + 纹理，返回纹理 / FBO（附着到 COLOR_ATTACHMENT0）。
// 不改变调用方当前 FBO 绑定（调用方负责在合适时机恢复，见各调用点）。
bool CreateRgba32fTarget(int w, int h, unsigned int* out_tex /*output*/,
                         unsigned int* out_fbo /*output*/, std::string* err /*output*/) {
    glGenTextures(1, out_tex);
    glBindTexture(GL_TEXTURE_2D, *out_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, out_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *out_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *out_tex, 0);
    const GLenum draw_buf[1] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(1, draw_buf);
    const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        *err = "froxel FBO incomplete";
        return false;
    }
    return true;
}

}  // anonymous namespace

namespace jpov {

FireFogRenderer::~FireFogRenderer() {
    Finalize();
}

void FireFogRenderer::Init(ShaderManager* shader_mgr) {
    CHECK(shader_mgr != nullptr);
    shader_mgr_ = shader_mgr;
    fs_inject_ = AssembleFragmentShader(kFireFogInjectBody);
    fs_reduce_ = AssembleFragmentShader(kFireFogReduceBody);
    fs_scatter_ = AssembleFragmentShader(kFireFogScatterBody);
    fs_composite_ = AssembleFragmentShader(kFireFogCompositeBody);
    prog_inject_ = shader_mgr_->GetOrCreate(
        "fire_fog_inject", {kFireFogVs, fs_inject_.c_str()});
    prog_reduce_ = shader_mgr_->GetOrCreate(
        "fire_fog_reduce", {kFireFogVs, fs_reduce_.c_str()});
    prog_scatter_ = shader_mgr_->GetOrCreate(
        "fire_fog_scatter", {kFireFogVs, fs_scatter_.c_str()});
    prog_composite_ = shader_mgr_->GetOrCreate(
        "fire_fog_composite", {kFireFogVs, fs_composite_.c_str()});

    // 1×1 深度哑元（值=1.0）：供调用方传 scene_depth_tex==0 时使用。
    const float one = 1.0f;
    glGenTextures(1, &dummy_depth_tex_);
    glBindTexture(GL_TEXTURE_2D, dummy_depth_tex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, 1, 1, 0, GL_RED, GL_FLOAT, &one);
    glBindTexture(GL_TEXTURE_2D, 0);

    // 1×1×1 RG8 哑元 2D 数组纹理（值 0）：未上传共享火纹理时绑定，采样退化为「无场」。
    const unsigned char zero_rg[2] = {0, 0};
    glGenTextures(1, &dummy_vol_tex_);
    glBindTexture(GL_TEXTURE_2D_ARRAY, dummy_vol_tex_);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RG8, 1, 1, 1, 0, GL_RG, GL_UNSIGNED_BYTE, zero_rg);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
}

void FireFogRenderer::Finalize() {
    if (inject_tex_ != 0) {
        glDeleteTextures(1, &inject_tex_);
        inject_tex_ = 0;
    }
    if (scatter_tex_ != 0) {
        glDeleteTextures(1, &scatter_tex_);
        scatter_tex_ = 0;
    }
    if (inject_fbo_ != 0) {
        glDeleteFramebuffers(1, &inject_fbo_);
        inject_fbo_ = 0;
    }
    if (scatter_fbo_ != 0) {
        glDeleteFramebuffers(1, &scatter_fbo_);
        scatter_fbo_ = 0;
    }
    for (int j = 0; j < kMaxLevels; ++j) {
        if (level_tex_[j] != 0) {
            glDeleteTextures(1, &level_tex_[j]);
            level_tex_[j] = 0;
        }
        if (level_fbo_[j] != 0) {
            glDeleteFramebuffers(1, &level_fbo_[j]);
            level_fbo_[j] = 0;
        }
        level_w_[j] = 0;
        level_h_[j] = 0;
    }
    num_levels_ = 0;
    level_sblock_ = 0;
    level_grid_w_ = 0;
    level_grid_h_ = 0;
    if (tile_index_tex_ != 0) {
        glDeleteTextures(1, &tile_index_tex_);
        tile_index_tex_ = 0;
    }
    if (tile_light_tex_ != 0) {
        glDeleteTextures(1, &tile_light_tex_);
        tile_light_tex_ = 0;
    }
    tile_light_tex_w_ = 0;
    tile_light_tex_h_ = 0;
    if (tile_zrange_tex_ != 0) {
        glDeleteTextures(1, &tile_zrange_tex_);
        tile_zrange_tex_ = 0;
    }
    tile_zrange_tex_w_ = 0;
    tile_zrange_tex_h_ = 0;
    if (tile_ray_tex_ != 0) {
        glDeleteTextures(1, &tile_ray_tex_);
        tile_ray_tex_ = 0;
    }
    tile_ray_tex_w_ = 0;
    tile_ray_tex_h_ = 0;
    if (z_slices_tex_ != 0) {
        glDeleteTextures(1, &z_slices_tex_);
        z_slices_tex_ = 0;
    }
    z_slices_count_ = 0;
    if (fog_body_tex_ != 0) {
        glDeleteTextures(1, &fog_body_tex_);
        fog_body_tex_ = 0;
    }
    if (dummy_depth_tex_ != 0) {
        glDeleteTextures(1, &dummy_depth_tex_);
        dummy_depth_tex_ = 0;
    }
    if (fire_vol_tex_ != 0) {
        glDeleteTextures(1, &fire_vol_tex_);
        fire_vol_tex_ = 0;
    }
    fire_vol_nx_ = 0;
    fire_vol_ny_ = 0;
    fire_vol_nz_ = 0;
    fire_vol_frames_ = 0;
    if (dummy_vol_tex_ != 0) {
        glDeleteTextures(1, &dummy_vol_tex_);
        dummy_vol_tex_ = 0;
    }
    froxel_w_ = 0;
    froxel_h_ = 0;
    tile_tex_w_ = 0;
    tile_tex_h_ = 0;
    grid_w_ = 0;
    grid_h_ = 0;
    bodies_.clear();
    prog_inject_ = 0;
    prog_reduce_ = 0;
    prog_scatter_ = 0;
    prog_composite_ = 0;
    shader_mgr_ = nullptr;
}

void FireFogRenderer::SetFireVolume(const FireVolume& vol) {
    CHECK(!vol.empty()) << "SetFireVolume: 体积数据为空";
    CHECK_GT(vol.nx, 0) << "SetFireVolume: nx 必须 > 0";
    CHECK_GT(vol.ny, 0) << "SetFireVolume: ny 必须 > 0";
    CHECK_GT(vol.nz, 0) << "SetFireVolume: nz 必须 > 0";
    CHECK_GE(vol.nframes, 1) << "SetFireVolume: nframes 必须 >= 1";
    CHECK_EQ(vol.channels, 2) << "SetFireVolume: 只支持 2 通道（R=density, G=temperature）";

    const size_t want = static_cast<size_t>(vol.nx) * vol.ny * vol.nz * vol.nframes * vol.channels;
    CHECK_EQ(vol.uint8 ? vol.bytes.size() : vol.data.size(), want)
        << "SetFireVolume: 数据长度与维数×通道不符";

    if (fire_vol_tex_ == 0) {
        glGenTextures(1, &fire_vol_tex_);
    }
    // 2D 数组：layer = 帧；每层 = nx×(ny*nz)（行 = z*ny + y，行内 x 最快）。layers = nframes。
    const int layers = vol.nframes;
    CHECK_LE(layers, kMaxFireVolLayers) << "SetFireVolume: 帧数 " << layers
                                        << " 超过数组层上限 " << kMaxFireVolLayers;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, fire_vol_tex_);
    // 层内 x/y 双线性（z 与帧的插值在采样器里手写）；边缘 clamp。
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    const int layer_h = vol.ny * vol.nz;
    if (vol.uint8) {
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RG8, vol.nx, layer_h, layers, 0,
                     GL_RG, GL_UNSIGNED_BYTE, vol.bytes.data());
    } else {
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RG32F, vol.nx, layer_h, layers, 0,
                     GL_RG, GL_FLOAT, vol.data.data());
    }
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    fire_vol_nx_ = vol.nx;
    fire_vol_ny_ = vol.ny;
    fire_vol_nz_ = vol.nz;
    fire_vol_frames_ = vol.nframes;
}

void FireFogRenderer::EnsureTileTexture(int grid_cols, int grid_rows) {
    CHECK_GE(grid_cols, 1);
    CHECK_GE(grid_rows, 1);
    const int tex_w = grid_cols * kTexelsPerTile;
    const int tex_h = grid_rows;
    const bool need_rebuild = (tile_index_tex_ == 0) || (tex_w != tile_tex_w_) ||
                              (tex_h != tile_tex_h_);
    if (need_rebuild) {
        if (tile_index_tex_ != 0) {
            glDeleteTextures(1, &tile_index_tex_);
        }
        glGenTextures(1, &tile_index_tex_);
        glBindTexture(GL_TEXTURE_2D, tile_index_tex_);
        // 无 mip；严格最近采样（整数 index 不可插值）；边缘 clamp。
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex_w, tex_h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
        tile_tex_w_ = tex_w;
        tile_tex_h_ = tex_h;
    }
    // z 范围纹理尺寸 = Nxy 栅格；tile_px 改变会改栅格 ⇒ 必须随尺寸重建
    // （原先只在 ==0 时建一次，切到更细的 tile 会残留旧尺寸 → 上传被拒 + 采样错位）。
    const bool zrange_need_rebuild = (tile_zrange_tex_ == 0) ||
                                     (grid_cols != tile_zrange_tex_w_) ||
                                     (grid_rows != tile_zrange_tex_h_);
    if (zrange_need_rebuild) {
        if (tile_zrange_tex_ != 0) {
            glDeleteTextures(1, &tile_zrange_tex_);
        }
        glGenTextures(1, &tile_zrange_tex_);
        glBindTexture(GL_TEXTURE_2D, tile_zrange_tex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG32F, grid_cols, grid_rows, 0,
                     GL_RG, GL_FLOAT, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
        tile_zrange_tex_w_ = grid_cols;
        tile_zrange_tex_h_ = grid_rows;
    }
    // tile 射线纹理：同样随 Nxy 栅格尺寸重建。（内容每帧在 Draw 里上传。）
    const bool ray_need_rebuild = (tile_ray_tex_ == 0) ||
                                  (grid_cols != tile_ray_tex_w_) ||
                                  (grid_rows != tile_ray_tex_h_);
    if (ray_need_rebuild) {
        if (tile_ray_tex_ != 0) {
            glDeleteTextures(1, &tile_ray_tex_);
        }
        glGenTextures(1, &tile_ray_tex_);
        glBindTexture(GL_TEXTURE_2D, tile_ray_tex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, grid_cols, grid_rows, 0,
                     GL_RGBA, GL_FLOAT, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
        tile_ray_tex_w_ = grid_cols;
        tile_ray_tex_h_ = grid_rows;
    }
    // 点光源 tile 索引纹理（RGBA8；宽 = grid_cols*kLightTexelsPerTile，高 = grid_rows）——
    // fire_fog **自持**一套光源 TC（不共用渲染器 16px 光表）；同样随 Nxy 栅格尺寸重建。
    const int light_tex_w = grid_cols * kLightTexelsPerTile;
    const bool light_need_rebuild = (tile_light_tex_ == 0) ||
                                    (light_tex_w != tile_light_tex_w_) ||
                                    (grid_rows != tile_light_tex_h_);
    if (light_need_rebuild) {
        if (tile_light_tex_ != 0) {
            glDeleteTextures(1, &tile_light_tex_);
        }
        glGenTextures(1, &tile_light_tex_);
        glBindTexture(GL_TEXTURE_2D, tile_light_tex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, light_tex_w, grid_rows, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
        tile_light_tex_w_ = light_tex_w;
        tile_light_tex_h_ = grid_rows;
    }
    grid_w_ = grid_cols;
    grid_h_ = grid_rows;
}

void FireFogRenderer::EnsureZSlices(int nz) {
    CHECK_GE(nz, 2);
    const int count = nz + 1;   // 边界 z_0..z_Nz
    if (z_slices_tex_ != 0 && z_slices_count_ == count) {
        return;
    }
    if (z_slices_tex_ != 0) {
        glDeleteTextures(1, &z_slices_tex_);
        z_slices_tex_ = 0;
    }
    glGenTextures(1, &z_slices_tex_);
    glBindTexture(GL_TEXTURE_2D, z_slices_tex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, count, 1, 0, GL_RED, GL_FLOAT, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
    z_slices_count_ = count;
}

void FireFogRenderer::EnsureTargets(int grid_cols, int grid_rows, int sblock,
                                    int num_levels) {
    CHECK_GE(grid_cols, 1);
    CHECK_GE(grid_rows, 1);
    CHECK_GE(sblock, 2);
    CHECK_GE(num_levels, 1);
    const int froxel_w = grid_cols * sblock;
    const int froxel_h = grid_rows * sblock;
    const bool rebuild = (inject_fbo_ == 0) || (froxel_w != froxel_w_) ||
                         (froxel_h != froxel_h_) || (num_levels != num_levels_) ||
                         (sblock != level_sblock_) || (grid_cols != level_grid_w_) ||
                         (grid_rows != level_grid_h_);
    if (!rebuild) {
        return;
    }
    GLint prev_binding = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_binding);

    // inject / scatter（主 FBO 尺寸）。
    unsigned int* texs[2] = {&inject_tex_, &scatter_tex_};
    unsigned int* fbos[2] = {&inject_fbo_, &scatter_fbo_};
    for (int s = 0; s < 2; ++s) {
        if (*fbos[s] != 0) {
            glDeleteFramebuffers(1, fbos[s]);
            *fbos[s] = 0;
        }
        if (*texs[s] != 0) {
            glDeleteTextures(1, texs[s]);
            *texs[s] = 0;
        }
        std::string err;
        const bool ok = CreateRgba32fTarget(froxel_w, froxel_h, texs[s], fbos[s], &err);
        CHECK(ok) << "FireFogRenderer: " << err
                  << "（RGBA32F 颜色附件不可渲染？w=" << froxel_w << " h=" << froxel_h << "）";
    }

    // 金字塔缩减级 1..num_levels：级 j 每柱边长 sblock>>j。
    for (int j = 0; j < kMaxLevels; ++j) {
        if (level_fbo_[j] != 0) {
            glDeleteFramebuffers(1, &level_fbo_[j]);
            level_fbo_[j] = 0;
        }
        if (level_tex_[j] != 0) {
            glDeleteTextures(1, &level_tex_[j]);
            level_tex_[j] = 0;
        }
        level_w_[j] = 0;
        level_h_[j] = 0;
    }
    for (int j = 1; j <= num_levels; ++j) {
        const int side = sblock >> j;
        const int w = grid_cols * side;
        const int h = grid_rows * side;
        std::string err;
        const bool ok =
            CreateRgba32fTarget(w, h, &level_tex_[j - 1], &level_fbo_[j - 1], &err);
        CHECK(ok) << "FireFogRenderer: 金字塔级 " << j << " " << err
                  << "（w=" << w << " h=" << h << "）";
        level_w_[j - 1] = w;
        level_h_[j - 1] = h;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_binding));
    froxel_w_ = froxel_w;
    froxel_h_ = froxel_h;
    num_levels_ = num_levels;
    level_sblock_ = sblock;
    level_grid_w_ = grid_cols;
    level_grid_h_ = grid_rows;
}

void FireFogRenderer::Draw(const std::vector<PointFog>& fogs,
                           const std::vector<FlameFog>& flame_fogs,
                           const std::vector<VolumeFog>& volume_fogs,
                           const std::vector<PointLight>& point_lights,
                           const Camera& cam,
                           const float view_proj[16],
                           int viewport_w,
                           int viewport_h,
                           unsigned int scene_depth_tex,
                           const FireFogParams& params,
                           const FireFogLighting& light) {
    CHECK(shader_mgr_ != nullptr) << "FireFogRenderer::Init 未调用";
    if (fogs.empty() && flame_fogs.empty() && volume_fogs.empty()) {
        return;   // 零开销
    }
    CHECK_GT(viewport_w, 0);
    CHECK_GT(viewport_h, 0);
    CHECK_GT(cam.near, 0.0f);
    CHECK_GT(cam.far, cam.near);
    CHECK_GT(params.z_near, 0.0f) << "FireFogParams.z_near 必须 > 0";
    CHECK_GT(params.z_far, params.z_near) << "FireFogParams.z_far 必须 > z_near";
    CHECK_GE(params.sun_phase_g, 0.0f);
    CHECK_LT(params.sun_phase_g, 0.95f) << "FireFogParams.sun_phase_g 必须 ∈ [0, 0.95)";
    // ── froxel 网格分辨率：nz（4 的幂）+ tile_px（屏幕像素）→ 派生量 ──
    //   sblock = √nz（须为 2 的幂，金字塔才能整齐 4 合 1）；
    //   Nxy    = ceil(W/tile_px) × ceil(H/tile_px)；
    //   froxel 纹理 = Nxy.x·sblock × Nxy.y·sblock。
    // 不合法直接 crash（不静默回退）。
    CHECK_GE(params.nz, kMinNz) << "FireFogParams.nz 必须 >= " << kMinNz;
    CHECK_LE(params.nz, kMaxNz) << "FireFogParams.nz 必须 <= " << kMaxNz;
    const int sblock =
        static_cast<int>(std::lround(std::sqrt(static_cast<double>(params.nz))));
    CHECK_EQ(sblock * sblock, params.nz)
        << "FireFogParams.nz 必须完全平方（sblock=√nz 需为整数），得到 " << params.nz;
    CHECK((sblock & (sblock - 1)) == 0)
        << "FireFogParams.nz 必须是 4 的幂（sblock=√nz 需为 2 的幂），得到 " << params.nz;
    int num_levels = 0;
    for (int s = sblock; s > 1; s >>= 1) {
        ++num_levels;
    }
    CHECK_GE(num_levels, 1);
    CHECK_LE(num_levels, kMaxLevels) << "金字塔级数 " << num_levels << " 超上限 " << kMaxLevels;
    CHECK_GE(params.tile_px, kMinTilePx) << "FireFogParams.tile_px 必须 >= " << kMinTilePx;
    CHECK_LE(params.tile_px, kMaxTilePx) << "FireFogParams.tile_px 必须 <= " << kMaxTilePx;
    const int grid_cols = (viewport_w + params.tile_px - 1) / params.tile_px;
    const int grid_rows = (viewport_h + params.tile_px - 1) / params.tile_px;
    const int froxel_w = grid_cols * sblock;
    const int froxel_h = grid_rows * sblock;
    CHECK_LE(froxel_w, kMaxFroxelDim) << "froxel 纹理宽 " << froxel_w << " 超上限 "
                                      << kMaxFroxelDim << "（调小 nz 或调大 tile_px）";
    CHECK_LE(froxel_h, kMaxFroxelDim) << "froxel 纹理高 " << froxel_h << " 超上限 "
                                      << kMaxFroxelDim << "（调小 nz 或调大 tile_px）";

    // ── 保存调用方状态（见头文件「GL 状态机契约」）──
    // ⚠️ 必须在任何可能重绑 FBO 的操作（Ensure*）**之前**取。
    GLint prev_fbo = 0;
    GLint prev_vp[4] = {0, 0, 0, 0};
    GLint prev_active_tex = 0;
    GLint prev_blend_src = 0;
    GLint prev_blend_dst = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_VIEWPORT, prev_vp);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active_tex);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &prev_blend_src);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &prev_blend_dst);
    const int units_touched = TexUnitsTouched();
    std::vector<GLint> prev_tex_binding(static_cast<size_t>(units_touched), 0);
    for (int u = 0; u < units_touched; ++u) {
        glActiveTexture(GL_TEXTURE0 + u);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex_binding[static_cast<size_t>(u)]);
    }
    // 共享 3D 火纹理绑定点（2D-array 绑定与 2D 分开，单独存取复原）。
    GLint prev_vol_binding = 0;
    glActiveTexture(GL_TEXTURE0 + kFireVolTexUnit);
    glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &prev_vol_binding);
    glActiveTexture(GL_TEXTURE0);

    // ── 命令层雾体（圆角盒 + 火焰 + 体积火）→ 后端统一团（FogBody）──
    //   三种子 command 先各自降维，再**合并**进同一份团列表（同一 inject pass over 合成）。
    const int round_count = static_cast<int>(fogs.size());
    const int flame_count = static_cast<int>(flame_fogs.size());
    const int vol_count = static_cast<int>(volume_fogs.size());
    int count = round_count + flame_count + vol_count;
    if (count > kMaxTotalFogs) {
        LOG(WARNING) << "point_fogs + flame_fogs + volume_fogs 数量 " << count
                     << " 超过上限 " << kMaxTotalFogs << "，仅前 " << kMaxTotalFogs
                     << " 个生效";
        count = kMaxTotalFogs;
    }
    bodies_.clear();
    bodies_.reserve(static_cast<size_t>(count));
    for (int i = 0; i < round_count && static_cast<int>(bodies_.size()) < count; ++i) {
        bodies_.push_back(LowerPointFog(fogs[static_cast<size_t>(i)]));
    }
    for (int i = 0; i < flame_count && static_cast<int>(bodies_.size()) < count; ++i) {
        bodies_.push_back(LowerFlameFog(flame_fogs[static_cast<size_t>(i)]));
    }
    for (int i = 0; i < vol_count && static_cast<int>(bodies_.size()) < count; ++i) {
        bodies_.push_back(LowerVolumeFog(volume_fogs[static_cast<size_t>(i)]));
    }

    // ── CPU 屏幕 tile 剪枝（每 tile ≤K 个团索引）+ 每 tile 保守 z 范围（GL-free）──
    EnsureTileTexture(grid_cols, grid_rows);
    const std::vector<uint8_t> tile_data = BuildTileFogIndexData(
        bodies_, view_proj, grid_w_, grid_h_, params.tile_px, kMaxFogsPerTile,
        kTexelsPerTile, kFogIndexSentinel);
    glBindTexture(GL_TEXTURE_2D, tile_index_tex_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tile_tex_w_, tile_tex_h_,
                    GL_RGBA, GL_UNSIGNED_BYTE, tile_data.data());
    const std::vector<float> zrange_data =
        BuildTileZRangeData(bodies_, view_proj, cam.position, grid_w_, grid_h_,
                            params.tile_px, params.z_near, params.z_far);
    glBindTexture(GL_TEXTURE_2D, tile_zrange_tex_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, grid_w_, grid_h_,
                    GL_RG, GL_FLOAT, zrange_data.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    // ── 点光源 tile 剪枝（fire_fog 自持网格；每 Nxy ≤ kMaxLightsPerTile 个光源）──
    const std::vector<uint8_t> light_data = BuildTileLightIndexData(
        point_lights, view_proj, grid_w_, grid_h_, params.tile_px,
        kMaxLightsPerTile, kLightTexelsPerTile, kFogIndexSentinel);
    glBindTexture(GL_TEXTURE_2D, tile_light_tex_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tile_light_tex_w_, tile_light_tex_h_,
                    GL_RGBA, GL_UNSIGNED_BYTE, light_data.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    // ── 团属性纹理（RGBA32F；每团 kBodyTexelsPerFog texel，见 fog_body.h 的按作用域布局表）──
    //    布局：t0=(kind,sigma,albedo.r,albedo.g) t1=(albedo.b,emission.rgb)
    //          t2..t6 = kRoundBox union（容器 center/bound_half/axes/radius/atten）
    const int body_tex_w = kMaxTotalFogs * kBodyTexelsPerFog;
    if (fog_body_tex_ == 0) {
        glGenTextures(1, &fog_body_tex_);
        glBindTexture(GL_TEXTURE_2D, fog_body_tex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, body_tex_w, 1, 0,
                     GL_RGBA, GL_FLOAT, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    std::vector<float> body_attr(static_cast<size_t>(count) * kBodyTexelsPerFog * 4, 0.0f);
    for (int i = 0; i < count; ++i) {
        const FogBody& b = bodies_[i];
        const size_t base = static_cast<size_t>(i) * kBodyTexelsPerFog * 4;
        float* t0 = &body_attr[base + 0 * 4];
        float* t1 = &body_attr[base + 1 * 4];
        t0[0] = static_cast<float>(static_cast<uint8_t>(b.kind));   // kind
        t0[1] = b.sigma;
        t0[2] = b.albedo.r;
        t0[3] = b.albedo.g;
        t1[0] = b.albedo.b;
        t1[1] = b.emission.r;
        t1[2] = b.emission.g;
        t1[3] = b.emission.b;
        if (b.kind == FogFieldKind::kRoundBox) {
            float* t2 = &body_attr[base + 2 * 4];
            float* t3 = &body_attr[base + 3 * 4];
            float* t4 = &body_attr[base + 4 * 4];
            float* t5 = &body_attr[base + 5 * 4];
            float* t6 = &body_attr[base + 6 * 4];
            t2[0] = b.bound.center.x();
            t2[1] = b.bound.center.y();
            t2[2] = b.bound.center.z();
            t2[3] = b.bound.half_extent.x();
            t3[0] = b.bound.half_extent.y();
            t3[1] = b.bound.half_extent.z();
            t3[2] = b.bound.axis[0].x();
            t3[3] = b.bound.axis[0].y();
            t4[0] = b.bound.axis[0].z();
            t4[1] = b.bound.axis[1].x();
            t4[2] = b.bound.axis[1].y();
            t4[3] = b.bound.axis[1].z();
            t5[0] = b.bound.axis[2].x();
            t5[1] = b.bound.axis[2].y();
            t5[2] = b.bound.axis[2].z();
            t5[3] = b.params[1];               // radius（外侧圆角壳厚度）
            t6[0] = b.params[0];               // attenuation 枚举
        } else if (b.kind == FogFieldKind::kFlame) {
            // 字段 → flame union（t2..t7，见 fog_body.h 的按作用域布局表 / flame_param）。
            const float* fp = b.params;
            float* t2 = &body_attr[base + 2 * 4];
            float* t3 = &body_attr[base + 3 * 4];
            float* t4 = &body_attr[base + 4 * 4];
            float* t5 = &body_attr[base + 5 * 4];
            float* t6 = &body_attr[base + 6 * 4];
            float* t7 = &body_attr[base + 7 * 4];
            t2[0] = fp[flame_param::kContX];
            t2[1] = fp[flame_param::kContY];
            t2[2] = fp[flame_param::kContZ];
            t2[3] = fp[flame_param::kContHx];
            t3[0] = fp[flame_param::kContHy];
            t3[1] = fp[flame_param::kContHz];
            t3[2] = fp[flame_param::kCenterX];
            t3[3] = fp[flame_param::kCenterY];
            t4[0] = fp[flame_param::kCenterZ];
            t4[1] = fp[flame_param::kBoxHx];
            t4[2] = fp[flame_param::kBoxHy];
            t4[3] = fp[flame_param::kBoxHz];
            t5[0] = fp[flame_param::kWindX];
            t5[1] = fp[flame_param::kWindZ];
            t5[2] = fp[flame_param::kLean];
            t5[3] = fp[flame_param::kBend];
            t6[0] = fp[flame_param::kShell];
            t6[1] = fp[flame_param::kPlume];
            t6[2] = fp[flame_param::kVigor];
            t6[3] = fp[flame_param::kExpand];
            t7[0] = fp[flame_param::kDecay];
        } else if (b.kind == FogFieldKind::kVolumeTex) {
            // 字段 → volumetex union（t2..t6，见 fog_body.h 的按作用域布局表 / volume_param）。
            // 盒为轴对齐（MVP）：轴取单位基，几何进 center/bound_h*。
            const float* vp = b.params;
            float* t2 = &body_attr[base + 2 * 4];
            float* t3 = &body_attr[base + 3 * 4];
            float* t4 = &body_attr[base + 4 * 4];
            float* t5 = &body_attr[base + 5 * 4];
            float* t6 = &body_attr[base + 6 * 4];
            t2[0] = vp[volume_param::kCenterX];
            t2[1] = vp[volume_param::kCenterY];
            t2[2] = vp[volume_param::kCenterZ];
            t2[3] = vp[volume_param::kHalfX];
            t3[0] = vp[volume_param::kHalfY];
            t3[1] = vp[volume_param::kHalfZ];
            t3[2] = 1.0f;   // axis0_x
            t3[3] = 0.0f;   // axis0_y
            t4[0] = 0.0f;   // axis0_z
            t4[1] = 0.0f;   // axis1_x
            t4[2] = 1.0f;   // axis1_y
            t4[3] = 0.0f;   // axis1_z
            t5[0] = 0.0f;   // axis2_x
            t5[1] = 0.0f;   // axis2_y
            t5[2] = 1.0f;   // axis2_z
            t5[3] = vp[volume_param::kDensityPow];
            t6[0] = vp[volume_param::kTempScale];
        }
    }
    glBindTexture(GL_TEXTURE_2D, fog_body_tex_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, count * kBodyTexelsPerFog, 1,
                    GL_RGBA, GL_FLOAT, body_attr.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    EnsureTargets(grid_cols, grid_rows, sblock, num_levels);
    EnsureZSlices(params.nz);

    float inv_vp[16];
    CHECK(Mat4Invert(view_proj, inv_vp))
        << "FireFogRenderer: 相机矩阵不可逆（near/far 非法或退化）";

    // ── 预烘两张小纹理：把 inject 原本**逐 texel** 的昂贵标量运算挪到 CPU / 每 tile 一次 ──
    //   ① 每 tile 中心视线方向（Nxy×1，单位向量）：省掉 inject 每 texel 两次 uInvVP 乘 + normalize。
    const std::vector<float> tile_ray =
        BuildTileRayData(inv_vp, cam.position, grid_w_, grid_h_);
    glBindTexture(GL_TEXTURE_2D, tile_ray_tex_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, grid_w_, grid_h_,
                    GL_RGBA, GL_FLOAT, tile_ray.data());
    //   ② z 切片边界表 z_k（(Nz+1)×1）：省掉 inject 每 texel pow(uR, k)。
    const float growth = std::pow(params.z_far / params.z_near,
                                  1.0f / static_cast<float>(params.nz));
    std::vector<float> z_slices(static_cast<size_t>(params.nz) + 1);
    z_slices[0] = params.z_near;
    for (int k = 0; k < params.nz; ++k) {
        z_slices[static_cast<size_t>(k) + 1] = z_slices[static_cast<size_t>(k)] * growth;
    }
    glBindTexture(GL_TEXTURE_2D, z_slices_tex_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, params.nz + 1, 1,
                    GL_RED, GL_FLOAT, z_slices.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    // ═══════════ 趟 1：inject（逐 froxel 局部 (τ, S) → inject_tex_）═══════════
    glBindFramebuffer(GL_FRAMEBUFFER, inject_fbo_);
    glViewport(0, 0, froxel_w, froxel_h);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    glUseProgram(prog_inject_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tile_index_tex_);
    glUniform1i(shader_mgr_->GetUniform(prog_inject_, "uTileFogIndices"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tile_zrange_tex_);
    glUniform1i(shader_mgr_->GetUniform(prog_inject_, "uTileZRange"), 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, fog_body_tex_);
    glUniform1i(shader_mgr_->GetUniform(prog_inject_, "uFogBodyTex"), 2);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, tile_ray_tex_);
    glUniform1i(shader_mgr_->GetUniform(prog_inject_, "uTileRayTex"), 3);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, z_slices_tex_);
    glUniform1i(shader_mgr_->GetUniform(prog_inject_, "uZSlicesTex"), 4);
    glActiveTexture(GL_TEXTURE0 + kLightIndexTexUnit);
    glBindTexture(GL_TEXTURE_2D, tile_light_tex_);
    glUniform1i(shader_mgr_->GetUniform(prog_inject_, "uTileLightIndices"),
                kLightIndexTexUnit);
    // 共享 3D 火纹理（kVolumeTex 团采样；未上传则绑 1×1×1 哑元 ⇒ 采样为 0）。
    glActiveTexture(GL_TEXTURE0 + kFireVolTexUnit);
    glBindTexture(GL_TEXTURE_2D_ARRAY, fire_vol_tex_ != 0 ? fire_vol_tex_ : dummy_vol_tex_);
    glUniform1i(shader_mgr_->GetUniform(prog_inject_, "uFireVolTex"), kFireVolTexUnit);
    glUniform1i(shader_mgr_->GetUniform(prog_inject_, "uVolumeNY"), fire_vol_ny_ > 0 ? fire_vol_ny_ : 1);
    glUniform1i(shader_mgr_->GetUniform(prog_inject_, "uVolumeNZ"), fire_vol_nz_ > 0 ? fire_vol_nz_ : 1);
    glUniform1i(shader_mgr_->GetUniform(prog_inject_, "uVolumeFrames"),
                fire_vol_frames_ > 0 ? fire_vol_frames_ : 1);
    glUniform1f(shader_mgr_->GetUniform(prog_inject_, "uVolumeTime"), params.volume_time);
    glActiveTexture(GL_TEXTURE0);
    glUniform3f(shader_mgr_->GetUniform(prog_inject_, "uCamPos"), cam.position.x(),
                cam.position.y(), cam.position.z());
    glUniform1f(shader_mgr_->GetUniform(prog_inject_, "uZNear"), params.z_near);
    glUniform1i(shader_mgr_->GetUniform(prog_inject_, "uNz"), params.nz);
    glUniform1i(shader_mgr_->GetUniform(prog_inject_, "uBlockSide"), sblock);
    glUniform1i(shader_mgr_->GetUniform(prog_inject_, "uTilePx"), params.tile_px);
    glUniform1i(shader_mgr_->GetUniform(prog_inject_, "uTotalFogs"), count);

    // ── 物理光照 + CSM 阴影（**始终开启**；无「无光照」链路）──
    const int cascade_count =
        light.sun.has_value()
            ? std::min(light.shadow_cfg.cascade_count, ShadowConfig::kMaxCascades)
            : 0;
    glUniform3f(shader_mgr_->GetUniform(prog_inject_, "uAmbientColor"),
                light.ambient_color.r, light.ambient_color.g, light.ambient_color.b);
    glUniform1f(shader_mgr_->GetUniform(prog_inject_, "uAmbientIntensity"),
                light.ambient_intensity);
    glUniform1i(shader_mgr_->GetUniform(prog_inject_, "uHasSun"),
                light.sun.has_value() ? 1 : 0);
    glUniform1f(shader_mgr_->GetUniform(prog_inject_, "uSunPhaseG"), params.sun_phase_g);
    glUniform1f(shader_mgr_->GetUniform(prog_inject_, "uSunGain"), params.sun_gain);
    if (light.sun.has_value()) {
        glUniform3f(shader_mgr_->GetUniform(prog_inject_, "uSunColor"),
                    light.sun->color.r, light.sun->color.g, light.sun->color.b);
        glUniform1f(shader_mgr_->GetUniform(prog_inject_, "uSunIntensity"),
                    light.sun->intensity);
        // 太阳方向在 CPU 归一：shader 的 HG 相位假设已归一，省掉逐 texel normalize。
        const Vec3f sd = light.sun->direction;
        const float sl = std::sqrt(sd.x() * sd.x() + sd.y() * sd.y() + sd.z() * sd.z());
        CHECK_GT(sl, 1e-8f) << "FireFog: 太阳方向为零向量";
        glUniform3f(shader_mgr_->GetUniform(prog_inject_, "uSunDir"),
                    sd.x() / sl, sd.y() / sl, sd.z() / sl);
    } else {
        // 无太阳：清零（防止上一帧残留）。
        glUniform3f(shader_mgr_->GetUniform(prog_inject_, "uSunColor"), 0.0f, 0.0f, 0.0f);
        glUniform1f(shader_mgr_->GetUniform(prog_inject_, "uSunIntensity"), 0.0f);
        glUniform3f(shader_mgr_->GetUniform(prog_inject_, "uSunDir"), 0.0f, -1.0f, 0.0f);
    }
    glUniform1i(shader_mgr_->GetUniform(prog_inject_, "uCascadeCount"), cascade_count);
    if (cascade_count > 0) {
        CHECK_EQ(light.shadow_fbos.size(), static_cast<size_t>(cascade_count))
            << "FireFog: shadow_fbos 与 cascade_count 不一致";
        const ShadowConfig& sc = light.shadow_cfg;
        glUniform1fv(shader_mgr_->GetUniform(prog_inject_, "uCascadeRanges"),
                     cascade_count, sc.cascade_ranges);
        glUniform1fv(shader_mgr_->GetUniform(prog_inject_, "uShadowTexelWorld"),
                     ShadowConfig::kMaxCascades, light.shadow_texel_world);
        glUniform1fv(shader_mgr_->GetUniform(prog_inject_, "uShadowBiasCascade"),
                     ShadowConfig::kMaxCascades, sc.cascade_bias);
        glUniform1i(shader_mgr_->GetUniform(prog_inject_, "uShadowBiasOverride"),
                    sc.override_cascade_bias ? 1 : 0);
        glUniform1f(shader_mgr_->GetUniform(prog_inject_, "uShadowFadeStart"), sc.fade_start);
        glUniform1f(shader_mgr_->GetUniform(prog_inject_, "uShadowFadeEnd"), sc.fade_end);
        for (int c = 0; c < cascade_count; ++c) {
            const unsigned int unit =
                static_cast<unsigned int>(kShadowTexUnitBase) + static_cast<unsigned int>(c);
            glActiveTexture(GL_TEXTURE0 + unit);
            glBindTexture(GL_TEXTURE_2D, light.shadow_fbos[c].tex);
            const std::string n_unit = "uShadowMap[" + std::to_string(c) + "]";
            glUniform1i(shader_mgr_->GetUniform(prog_inject_, n_unit.c_str()),
                        static_cast<int>(unit));
            const std::string n_vp = "uShadowVP[" + std::to_string(c) + "]";
            glUniformMatrix4fv(shader_mgr_->GetUniform(prog_inject_, n_vp.c_str()),
                               1, GL_FALSE, light.shadow_vp[c]);
            const std::string n_dvp = "uShadowDepthVP[" + std::to_string(c) + "]";
            glUniformMatrix4fv(shader_mgr_->GetUniform(prog_inject_, n_dvp.c_str()),
                               1, GL_FALSE, light.shadow_depth_vp[c]);
        }
        glActiveTexture(GL_TEXTURE0);
    }

    // ── 点光源（tile culling；仅需内散射的 position/color/radius/intensity）──
    const int light_count =
        std::min(static_cast<int>(point_lights.size()), kMaxTotalLights);
    for (int i = 0; i < light_count; ++i) {
        const PointLight& pl = point_lights[i];
        const std::string n_pos = "uFogLights[" + std::to_string(i) + "].position";
        glUniform3f(shader_mgr_->GetUniform(prog_inject_, n_pos.c_str()),
                    pl.position.x(), pl.position.y(), pl.position.z());
        const std::string n_col = "uFogLights[" + std::to_string(i) + "].color";
        glUniform3f(shader_mgr_->GetUniform(prog_inject_, n_col.c_str()),
                    pl.color.r, pl.color.g, pl.color.b);
        const std::string n_rad = "uFogLights[" + std::to_string(i) + "].radius";
        glUniform1f(shader_mgr_->GetUniform(prog_inject_, n_rad.c_str()), pl.linear_radius);
        const std::string n_int = "uFogLights[" + std::to_string(i) + "].intensity";
        glUniform1f(shader_mgr_->GetUniform(prog_inject_, n_int.c_str()), pl.intensity);
    }
    glUniform1i(shader_mgr_->GetUniform(prog_inject_, "uTotalLights"), light_count);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // ═══════════ 趟 2：reduce（金字塔每级：上一级 z 上 4 合 1）═══════════
    glUseProgram(prog_reduce_);
    for (int j = 1; j <= num_levels; ++j) {
        const unsigned int src_tex = (j == 1) ? inject_tex_ : level_tex_[j - 2];
        const int src_side = sblock >> (j - 1);
        const int dst_side = sblock >> j;
        glBindFramebuffer(GL_FRAMEBUFFER, level_fbo_[j - 1]);
        glViewport(0, 0, level_w_[j - 1], level_h_[j - 1]);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, src_tex);
        glUniform1i(shader_mgr_->GetUniform(prog_reduce_, "uSrcLevel"), 0);
        glUniform1i(shader_mgr_->GetUniform(prog_reduce_, "uLevelSide"), dst_side);
        glUniform1i(shader_mgr_->GetUniform(prog_reduce_, "uSrcSide"), src_side);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    // ═══════════ 趟 3：scatter（金字塔 + base-4 有序前缀 → scatter_tex_）═══════════
    glBindFramebuffer(GL_FRAMEBUFFER, scatter_fbo_);
    glViewport(0, 0, froxel_w, froxel_h);
    glUseProgram(prog_scatter_);
    glUniform1i(shader_mgr_->GetUniform(prog_scatter_, "uNz"), params.nz);
    glUniform1i(shader_mgr_->GetUniform(prog_scatter_, "uBlockSide"), sblock);
    glUniform1i(shader_mgr_->GetUniform(prog_scatter_, "uTilePx"), params.tile_px);
    glUniform1i(shader_mgr_->GetUniform(prog_scatter_, "uLevelsCount"), num_levels);
    for (int j = 0; j <= num_levels; ++j) {
        const unsigned int tex = (j == 0) ? inject_tex_ : level_tex_[j - 1];
        glActiveTexture(GL_TEXTURE0 + j);
        glBindTexture(GL_TEXTURE_2D, tex);
        const std::string n_unit = "uLevels[" + std::to_string(j) + "]";
        glUniform1i(shader_mgr_->GetUniform(prog_scatter_, n_unit.c_str()), j);
    }
    glActiveTexture(GL_TEXTURE0);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // ═══════════ 趟 4：composite（全屏查表 → 就地混合进调用方 HDR FBO）═══════════
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
    glUseProgram(prog_composite_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scatter_tex_);
    glUniform1i(shader_mgr_->GetUniform(prog_composite_, "uScatter"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, scene_depth_tex != 0 ? scene_depth_tex : dummy_depth_tex_);
    glUniform1i(shader_mgr_->GetUniform(prog_composite_, "uSceneDepthTex"), 1);
    glActiveTexture(GL_TEXTURE0);
    glUniformMatrix4fv(shader_mgr_->GetUniform(prog_composite_, "uInvVP"), 1, GL_FALSE, inv_vp);
    glUniform3f(shader_mgr_->GetUniform(prog_composite_, "uCamPos"), cam.position.x(),
                cam.position.y(), cam.position.z());
    glUniform1f(shader_mgr_->GetUniform(prog_composite_, "uZNear"), params.z_near);
    glUniform1f(shader_mgr_->GetUniform(prog_composite_, "uZFar"), params.z_far);
    glUniform1f(shader_mgr_->GetUniform(prog_composite_, "uZScale"),
                static_cast<float>(params.nz) / std::log(params.z_far / params.z_near));
    glUniform1i(shader_mgr_->GetUniform(prog_composite_, "uGridCols"), grid_w_);
    glUniform1i(shader_mgr_->GetUniform(prog_composite_, "uGridRows"), grid_h_);
    glUniform1i(shader_mgr_->GetUniform(prog_composite_, "uNz"), params.nz);
    glUniform1i(shader_mgr_->GetUniform(prog_composite_, "uBlockSide"), sblock);
    glUniform1i(shader_mgr_->GetUniform(prog_composite_, "uTilePx"), params.tile_px);

    // 就地混合：out = S + dst·T（S=源累积亮度、T=透射率）。无雾像素 → 恒等。
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // ── 复原状态（调用方随后做 resolve / highlight / bloom / tone map / 2D，均自设状态）──
    // enable 位（blend/depth/cull）由调用方 glPushAttrib(GL_ENABLE_BIT) 兜底；此处仍显式关掉。
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    // ⭐ 回写混合函数：GL_BLEND_SRC/DST_ALPHA 属 GL_COLOR_BUFFER_BIT，**不在** enable 位里，
    //   必须显式复原成本趟进入前的值（否则 composite 设的 GL_ONE/GL_SRC_ALPHA 会沿用到 2D/字体）。
    glBlendFunc(static_cast<GLenum>(prev_blend_src), static_cast<GLenum>(prev_blend_dst));
    glUseProgram(0);
    // 回写本趟触碰过的纹理单元绑定（0..units_touched-1），再复原活动单元。
    for (int u = 0; u < units_touched; ++u) {
        glActiveTexture(GL_TEXTURE0 + u);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev_tex_binding[static_cast<size_t>(u)]));
    }
    // 复原共享火纹理单元的 2D-array 绑定（本趟在此单元绑过 2D 数组纹理）。
    glActiveTexture(GL_TEXTURE0 + kFireVolTexUnit);
    glBindTexture(GL_TEXTURE_2D_ARRAY, static_cast<GLuint>(prev_vol_binding));
    glActiveTexture(static_cast<GLenum>(prev_active_tex));
}

}  // namespace jpov
