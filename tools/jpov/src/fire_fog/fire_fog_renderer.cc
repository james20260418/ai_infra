// JPOV FireFogRenderer 实现
//
// 本 PR 落地 L1：把点状雾体降维成统一团（FogBody），投影其 OBB 到屏幕 tile 网格，
// 建 tile 索引表（每 tile ≤ K = 16，先到先得），并用一个调试 FS 把「有元素的 tile」
// 涂一层 color blend 就地合成到当前绑定的 FBO（验证剪枝正确性）。
// L2（逐像素 ZDist + 末端积分）见 docs/jpov_fire_fog_design.md §10.6。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/fire_fog/fire_fog_renderer.h"

#include <algorithm>
#include <cmath>
#include <vector>

// GL 头文件必须最先 include（在 MinGW #define 宏替换之前）
#ifdef _WIN32
#include <GL/gl.h>
#else
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#ifdef _WIN32
// MinGW: windows.h 定义 ERROR 宏与 glog 冲突，必须在 glog 之前 suppress
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
#include "third_party/gl_loader-mingw/gl_loader.h"
#endif

// Windows/MinGW: windef.h 定义 near/far 宏，与 Camera::near/far 字段冲突。
#ifdef _WIN32
#undef near
#undef far
#endif

#include <glog/logging.h>

namespace jpov {

namespace {

// 全屏三角形 VS（无 VAO/VBO，用 gl_VertexID 覆盖 NDC）——同 tone map pass 的模式。
const char* kTileDebugVs = R"glsl(
#version 330 core
void main() {
    vec2 pos;
    if (gl_VertexID == 0) pos = vec2(-1.0, -1.0);
    else if (gl_VertexID == 1) pos = vec2( 3.0, -1.0);
    else pos = vec2(-1.0,  3.0);
    gl_Position = vec4(pos, 0.0, 1.0);
}
)glsl";

// 调试 FS：把「有雾体覆盖的 tile」涂一层半透明色（验证 L1 tile 剪枝）。
// tile 索引纹理布局：每个 tile 占 K 个连续列（x = tile_col*K .. +K-1），槽值 >0 ⇒ 有元素；
// 因先到先得按槽序填，故查「槽 0」即可判定该 tile 是否非空。
const char* kTileDebugFs = R"glsl(
#version 330 core
out vec4 fragColor;
uniform sampler2D uTileFogIndices;
uniform int uTileSize;
uniform int uFogsPerTile;
uniform vec4 uTileColor;
void main() {
    ivec2 tex = textureSize(uTileFogIndices, 0);
    int grid_cols = tex.x / uFogsPerTile;
    int grid_rows = tex.y;
    ivec2 fc = ivec2(gl_FragCoord.xy);
    int tc = clamp(fc.x / uTileSize, 0, grid_cols - 1);
    int tr = clamp(fc.y / uTileSize, 0, grid_rows - 1);
    float slot0 = texelFetch(uTileFogIndices, ivec2(tc * uFogsPerTile, tr), 0).r;
    if (slot0 > 0.5) {
        fragColor = uTileColor;
    } else {
        discard;
    }
}
)glsl";

// 用列主序 mvp 投影世界点 p 到屏幕像素坐标（x 右、y 向上，与 gl_FragCoord 同向）。
// 返回 false 表示该点在相机背后（w <= 0，投影无效）。
bool ProjectToScreen(const float mvp[16], const Vec3f& p, int fbo_w, int fbo_h,
                     float* out_x /*output*/, float* out_y /*output*/) {
    const float w = mvp[3]*p.x() + mvp[7]*p.y() + mvp[11]*p.z() + mvp[15];
    if (w <= 0.0f) {
        return false;
    }
    const float inv = 1.0f / w;
    const float nx = (mvp[0]*p.x() + mvp[4]*p.y() + mvp[8] *p.z() + mvp[12]) * inv;
    const float ny = (mvp[1]*p.x() + mvp[5]*p.y() + mvp[9] *p.z() + mvp[13]) * inv;
    *out_x = (nx * 0.5f + 0.5f) * static_cast<float>(fbo_w);
    *out_y = (ny * 0.5f + 0.5f) * static_cast<float>(fbo_h);
    return true;
}

}  // namespace

FireFogRenderer::~FireFogRenderer() {
    Finalize();
}

void FireFogRenderer::Init(ShaderManager* shader_mgr) {
    CHECK(shader_mgr != nullptr);
    shader_mgr_ = shader_mgr;
}

void FireFogRenderer::Finalize() {
    if (tile_index_tex_ != 0) {
        glDeleteTextures(1, &tile_index_tex_);
        tile_index_tex_ = 0;
    }
    tile_grid_w_ = 0;
    tile_grid_h_ = 0;
    tile_tex_w_ = 0;
    tile_tex_h_ = 0;
    bodies_.clear();
    shader_mgr_ = nullptr;
}

void FireFogRenderer::Draw(const std::vector<PointFog>& fogs,
                           bool debug_visualize_tiles,
                           const Camera& cam,
                           const float view_proj[16],
                           int viewport_w,
                           int viewport_h,
                           unsigned int /*scene_depth_tex*/) {
    // M1a 用 cam 重建光线；本 PR 的 tile 剪枝/调试不需要。
    (void)cam;
    if (fogs.empty() || viewport_w <= 0 || viewport_h <= 0) {
        return;
    }
    LowerToBodies(fogs);
    EnsureTileTable(viewport_w, viewport_h);
    BuildTileIndices(view_proj, viewport_w, viewport_h);
    if (debug_visualize_tiles) {
        DrawTileDebugOverlay();
    }
}

void FireFogRenderer::LowerToBodies(const std::vector<PointFog>& fogs) {
    bodies_.clear();
    bodies_.reserve(fogs.size());
    for (const PointFog& f : fogs) {
        CHECK_GT(f.radius, 0.0f) << "PointFog.radius 必须 > 0";
        FogBody body{};
        body.bound.center = f.center;
        body.bound.axis[0] = Vec3f{1.0f, 0.0f, 0.0f};
        body.bound.axis[1] = Vec3f{0.0f, 1.0f, 0.0f};
        body.bound.axis[2] = Vec3f{0.0f, 0.0f, 1.0f};
        body.bound.half_extent = Vec3f{f.radius, f.radius, f.radius};
        body.kind = FogFieldKind::kAnalyticProfile;
        body.color = f.color;
        body.intensity = f.intensity;
        // M1a：真实消光由 intensity/颜色与剖面推出；本 PR（tile 验证）不使用。
        body.sigma_scale = 1.0f;
        body.params[0] = static_cast<float>(f.attenuation);
        body.prebaked_lin = Vec3f{0.0f, 0.0f, 0.0f};
        bodies_.push_back(body);
    }
}

void FireFogRenderer::EnsureTileTable(int viewport_w, int viewport_h) {
    const int grid_cols = (viewport_w + kTileSize - 1) / kTileSize;
    const int grid_rows = (viewport_h + kTileSize - 1) / kTileSize;
    if (tile_index_tex_ != 0 && tile_grid_w_ == grid_cols && tile_grid_h_ == grid_rows) {
        return;
    }
    if (tile_index_tex_ != 0) {
        glDeleteTextures(1, &tile_index_tex_);
        tile_index_tex_ = 0;
    }
    tile_grid_w_ = grid_cols;
    tile_grid_h_ = grid_rows;
    tile_tex_w_ = grid_cols * kMaxFogsPerTile;
    tile_tex_h_ = grid_rows;

    glGenTextures(1, &tile_index_tex_);
    glBindTexture(GL_TEXTURE_2D, tile_index_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, tile_tex_w_, tile_tex_h_, 0,
                 GL_RED, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void FireFogRenderer::BuildTileIndices(const float view_proj[16],
                                       int fbo_w, int fbo_h) {
    if (tile_index_tex_ == 0 || tile_grid_w_ <= 0 || tile_grid_h_ <= 0) {
        return;
    }
    const int total_tiles = tile_grid_w_ * tile_grid_h_;
    std::vector<float> slots(static_cast<size_t>(tile_tex_w_) * tile_tex_h_, 0.0f);
    std::vector<int> counts(static_cast<size_t>(total_tiles), 0);

    for (int bi = 0; bi < static_cast<int>(bodies_.size()); ++bi) {
        const Obb& box = bodies_[bi].bound;

        // 投影 OBB 的 8 个角点到屏幕，取包围矩形（Obb ⊇ 球 ⇒ 其 8 角投影包围盒 ⊇ 球投影，
        // 保守不漏）。任一角在相机背后则退化为「保守全覆盖」。
        float min_x = 0.0f, max_x = 0.0f, min_y = 0.0f, max_y = 0.0f;
        bool have_point = false;
        bool crosses_camera = false;
        for (int s = 0; s < 8; ++s) {
            const float sx = (s & 1) ? 1.0f : -1.0f;
            const float sy = (s & 2) ? 1.0f : -1.0f;
            const float sz = (s & 4) ? 1.0f : -1.0f;
            const float ex = box.half_extent.x() * sx;
            const float ey = box.half_extent.y() * sy;
            const float ez = box.half_extent.z() * sz;
            const Vec3f corner{
                box.center.x() + box.axis[0].x()*ex + box.axis[1].x()*ey + box.axis[2].x()*ez,
                box.center.y() + box.axis[0].y()*ex + box.axis[1].y()*ey + box.axis[2].y()*ez,
                box.center.z() + box.axis[0].z()*ex + box.axis[1].z()*ey + box.axis[2].z()*ez,
            };
            float px, py;
            if (!ProjectToScreen(view_proj, corner, fbo_w, fbo_h, &px, &py)) {
                crosses_camera = true;
                continue;
            }
            if (!have_point) {
                min_x = max_x = px;
                min_y = max_y = py;
                have_point = true;
            } else {
                min_x = std::min(min_x, px);
                max_x = std::max(max_x, px);
                min_y = std::min(min_y, py);
                max_y = std::max(max_y, py);
            }
        }

        int min_px, max_px, min_py, max_py;
        if (crosses_camera || !have_point) {
            // 保守：覆盖全屏（与点光源 culling「球跨相机 → 全屏」同规则）。
            min_px = 0;
            max_px = fbo_w;
            min_py = 0;
            max_py = fbo_h;
        } else {
            // ±1 余量防边界舍入（宁多勿少）。
            min_px = static_cast<int>(std::floor(min_x)) - 1;
            max_px = static_cast<int>(std::ceil(max_x)) + 1;
            min_py = static_cast<int>(std::floor(min_y)) - 1;
            max_py = static_cast<int>(std::ceil(max_y)) + 1;
        }

        const int min_tc = std::max(0, min_px / kTileSize);
        const int max_tc = std::min(tile_grid_w_ - 1, max_px / kTileSize);
        const int min_tr = std::max(0, min_py / kTileSize);
        const int max_tr = std::min(tile_grid_h_ - 1, max_py / kTileSize);
        if (min_tc > max_tc || min_tr > max_tr) {
            continue;
        }

        // 向覆盖的每个 tile 写入雾 index（先到先得，每 tile ≤ K）。
        for (int tr = min_tr; tr <= max_tr; ++tr) {
            for (int tc = min_tc; tc <= max_tc; ++tc) {
                const int t = tr * tile_grid_w_ + tc;
                if (counts[t] >= kMaxFogsPerTile) {
                    continue;
                }
                const int slot = counts[t];
                slots[static_cast<size_t>(tr) * tile_tex_w_ + tc * kMaxFogsPerTile + slot] =
                    static_cast<float>(bi + 1);
                ++counts[t];
            }
        }
    }

    glBindTexture(GL_TEXTURE_2D, tile_index_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, tile_tex_w_, tile_tex_h_, 0,
                 GL_RED, GL_FLOAT, slots.data());
    glBindTexture(GL_TEXTURE_2D, 0);
}

unsigned int FireFogRenderer::TileDebugProg() {
    if (shader_mgr_ == nullptr) {
        return 0;
    }
    return shader_mgr_->GetOrCreate("fire_fog_tile_debug",
                                    ShaderSource{kTileDebugVs, kTileDebugFs});
}

void FireFogRenderer::DrawTileDebugOverlay() {
    if (tile_index_tex_ == 0) {
        return;
    }
    const unsigned int prog = TileDebugProg();
    if (prog == 0) {
        return;
    }
    glUseProgram(prog);
    // fire_fog 是独立 pass（独立 program，与 PBR/点光源不同时绑定），故复用纹理单元 0。
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tile_index_tex_);
    glUniform1i(shader_mgr_->GetUniform(prog, "uTileFogIndices"), 0);
    glUniform1i(shader_mgr_->GetUniform(prog, "uTileSize"), kTileSize);
    glUniform1i(shader_mgr_->GetUniform(prog, "uFogsPerTile"), kMaxFogsPerTile);
    glUniform4f(shader_mgr_->GetUniform(prog, "uTileColor"), 0.15f, 0.9f, 0.35f, 0.35f);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLES, 0, 3);   // 全屏三角形（gl_VertexID，无 VAO/VBO）
    glDisable(GL_BLEND);
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

}  // namespace jpov
