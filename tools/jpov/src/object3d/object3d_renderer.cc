// JPOV Object3DRenderer 实现
//
// PBR DrawObject3D + Tile Forward 光照（tile culling CPU 端 + 光源上传）。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/object3d/object3d_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
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

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#endif

#include <glog/logging.h>

namespace jpov {

namespace {

// 4x4 矩阵乘法：out = a * b（列主序）
void Mat4Mul(const float a[16], const float b[16], float out[16]) {
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a[k * 4 + row] * b[col * 4 + k];
            }
            out[col * 4 + row] = sum;
        }
    }
}

// 从 center/up/front 构建 Model 矩阵（列主序，纯 CPU，不碰 GL 矩阵栈）
void BuildModelMatrix(const Vec3f& center,
                      const Vec3f& up,
                      const Vec3f& front,
                      float model[16]) {
    float u_len = std::sqrt(up.x()*up.x() + up.y()*up.y() + up.z()*up.z());
    float f_len = std::sqrt(front.x()*front.x() + front.y()*front.y() + front.z()*front.z());
    Vec3f upn = {up.x()/u_len, up.y()/u_len, up.z()/u_len};
    Vec3f frn = {front.x()/f_len, front.y()/f_len, front.z()/f_len};

    Vec3f right = {upn.y()*frn.z() - upn.z()*frn.y(),
                   upn.z()*frn.x() - upn.x()*frn.z(),
                   upn.x()*frn.y() - upn.y()*frn.x()};
    float r_len = std::sqrt(right.x()*right.x() + right.y()*right.y() + right.z()*right.z());
    right = {right.x()/r_len, right.y()/r_len, right.z()/r_len};

    model[0] = right.x(); model[4] = upn.x(); model[8]  = frn.x(); model[12] = center.x();
    model[1] = right.y(); model[5] = upn.y(); model[9]  = frn.y(); model[13] = center.y();
    model[2] = right.z(); model[6] = upn.z(); model[10] = frn.z(); model[14] = center.z();
    model[3] = 0.0f;      model[7] = 0.0f;    model[11] = 0.0f;    model[15] = 1.0f;
}

}  // namespace

// ==================== EnsureTileLighting ====================

void Object3DRenderer::EnsureTileLighting(int fbo_w, int fbo_h,
                                          unsigned int* tile_index_tex /*output*/,
                                          int* grid_w /*output*/,
                                          int* grid_h /*output*/,
                                          int* tex_w /*output*/,
                                          int* tex_h /*output*/) {
    int grid_cols = (fbo_w + kTileSize16 - 1) / kTileSize16;
    int grid_rows = (fbo_h + kTileSize16 - 1) / kTileSize16;

    int tw = grid_cols * kTexelsPerTile;
    int th = grid_rows;

    bool need_create =
        (*tile_index_tex == 0) || (*grid_w != grid_cols) || (*grid_h != grid_rows);

    if (need_create) {
        if (*tile_index_tex) {
            glDeleteTextures(1, tile_index_tex);
        }
        glGenTextures(1, tile_index_tex);
        glBindTexture(GL_TEXTURE_2D, *tile_index_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    *grid_w = grid_cols;
    *grid_h = grid_rows;
    *tex_w = tw;
    *tex_h = th;
}

// ==================== UploadLightData ====================

void Object3DRenderer::UploadLightData(const RenderCommandList& cmds,
                                       ShaderManager& shader_mgr,
                                       unsigned int prog,
                                       unsigned int prog_full) {
    const unsigned int programs[] = {prog, prog_full};

    const int total = static_cast<int>(cmds.point_lights.size());
    const int clamped = std::min(total, kMaxTotalLights);

    char buf[64];
    for (unsigned int p : programs) {
        glUseProgram(p);
        for (int i = 0; i < clamped; ++i) {
            const PointLight& l = cmds.point_lights[i];
            snprintf(buf, sizeof(buf), "uLights[%d].position", i);
            glUniform3f(shader_mgr.GetUniform(p, buf),
                        l.position.x(), l.position.y(), l.position.z());
            snprintf(buf, sizeof(buf), "uLights[%d].color", i);
            glUniform3f(shader_mgr.GetUniform(p, buf),
                        l.color.r, l.color.g, l.color.b);
            snprintf(buf, sizeof(buf), "uLights[%d].radius", i);
            glUniform1f(shader_mgr.GetUniform(p, buf), l.linear_radius);
            snprintf(buf, sizeof(buf), "uLights[%d].physicalRadius", i);
            glUniform1f(shader_mgr.GetUniform(p, buf), l.physical_radius);
            snprintf(buf, sizeof(buf), "uLights[%d].intensity", i);
            glUniform1f(shader_mgr.GetUniform(p, buf), l.intensity);
        }
        glUniform1i(shader_mgr.GetUniform(p, "uTotalLights"), clamped);
        // uTileCulling=1（默认）：片元走 tile 索引纹理；=0：遍历全部光源。
        glUniform1i(shader_mgr.GetUniform(p, "uTileCulling"),
                    cmds.tile_culling ? 1 : 0);
    }
}

// ==================== UploadAmbient ====================

void Object3DRenderer::UploadAmbient(ShaderManager& shader_mgr,
                                     unsigned int prog,
                                     unsigned int prog_full,
                                     const AmbientLight& ambient) {
    const unsigned int programs[] = {prog, prog_full};
    for (unsigned int p : programs) {
        glUseProgram(p);
        glUniform3f(shader_mgr.GetUniform(p, "uAmbientColor"),
                    ambient.color.r, ambient.color.g, ambient.color.b);
        glUniform1f(shader_mgr.GetUniform(p, "uAmbientIntensity"),
                    std::max(ambient.intensity, 0.0f));  // 负值 clamp 到 0
    }
}

// ==================== BuildTileLightIndices ====================

void Object3DRenderer::BuildTileLightIndices(const RenderCommandList& cmds,
                                             int fbo_w, int fbo_h,
                                             unsigned int tile_index_tex,
                                             int grid_w, int grid_h,
                                             int tex_w, int tex_h,
                                             const float mvp[16]) {
    if (tile_index_tex == 0) {
        return;
    }
    if (grid_w <= 0 || grid_h <= 0) {
        return;
    }

    const int num_lights = static_cast<int>(cmds.point_lights.size());
    if (num_lights == 0) {
        return;
    }

    if (num_lights > kMaxTotalLights) {
        LOG_FIRST_N(WARNING, 1)
            << "point_lights 数量 " << num_lights << " 超过上限 "
            << kMaxTotalLights
            << "，仅前 " << kMaxTotalLights << " 个光源生效";
    }

    // 1. 准备 tile 网格的 CPU 侧缓冲
    const int total_tiles = grid_w * grid_h;
    std::vector<uint32_t> tile_counts(static_cast<size_t>(total_tiles), 0);
    struct Cell {
        uint8_t idx[kMaxLightsPerTile];
    };
    std::vector<Cell> tiles(static_cast<size_t>(total_tiles));
    for (Cell& c : tiles) {
        for (int i = 0; i < kMaxLightsPerTile; ++i) {
            c.idx[i] = kLightIndexSentinel;
        }
    }

    // 2. 遍历光源（用户已按优先级排好序，先到先得）
    const int clamped = std::min(num_lights, kMaxTotalLights);
    for (int li = 0; li < clamped; ++li) {
        const PointLight& l = cmds.point_lights[li];
        const Vec3f cx = {l.position.x(), l.position.y(), l.position.z()};
        const float radius = l.linear_radius;
        if (radius <= 0.0f) {
            continue;
        }

        // 投影球心到 clip 坐标
        float c[4];
        c[0] = mvp[0]*cx.x() + mvp[4]*cx.y() + mvp[8]*cx.z()  + mvp[12];
        c[1] = mvp[1]*cx.x() + mvp[5]*cx.y() + mvp[9]*cx.z()  + mvp[13];
        c[2] = mvp[2]*cx.x() + mvp[6]*cx.y() + mvp[10]*cx.z() + mvp[14];
        c[3] = mvp[3]*cx.x() + mvp[7]*cx.y() + mvp[11]*cx.z() + mvp[15];

        if (c[3] <= 0.0f) {
            continue;
        }

        const float inv_w = 1.0f / c[3];
        const float ndc_x = c[0] * inv_w;
        const float ndc_y = c[1] * inv_w;

        const float px = (ndc_x * 0.5f + 0.5f) * static_cast<float>(fbo_w);
        const float py = (ndc_y * 0.5f + 0.5f) * static_cast<float>(fbo_h);

        float pmin_x = px;
        float pmax_x = px;
        float pmin_y = py;
        float pmax_y = py;
        bool crosses_camera = false;

        // 保守包围：投影光源影响球的世界空间 AABB 的 8 个角点（±radius 各方向）。
        // （之前只投 6 个轴向点会 undershoot：球投影是椭圆，off-axis 时 6 个
        //  轴向点的 bbox 漏掉真实轮廓 → 边缘 tile 无 light index → 漏光暗带。
        //  AABB ⊇ 球，故其 8 角投影 bbox 必 ⊇ 球投影，保证不漏 tile。）
        const Vec3f dirs[8] = {
            { 1, 1, 1}, { 1, 1,-1}, { 1,-1, 1}, { 1,-1,-1},
            {-1, 1, 1}, {-1, 1,-1}, {-1,-1, 1}, {-1,-1,-1},
        };
        for (int d = 0; d < 8; ++d) {
            const Vec3f bp = {
                cx.x() + dirs[d].x() * radius,
                cx.y() + dirs[d].y() * radius,
                cx.z() + dirs[d].z() * radius,
            };
            float bc[4];
            bc[0] = mvp[0]*bp.x() + mvp[4]*bp.y() + mvp[8]*bp.z()  + mvp[12];
            bc[1] = mvp[1]*bp.x() + mvp[5]*bp.y() + mvp[9]*bp.z()  + mvp[13];
            bc[2] = mvp[2]*bp.x() + mvp[6]*bp.y() + mvp[10]*bp.z() + mvp[14];
            bc[3] = mvp[3]*bp.x() + mvp[7]*bp.y() + mvp[11]*bp.z() + mvp[15];
            if (bc[3] <= 0.0f) {
                crosses_camera = true;
                continue;
            }
            const float biw = 1.0f / bc[3];
            const float bpx = (bc[0]*biw*0.5f + 0.5f) * static_cast<float>(fbo_w);
            const float bpy = (bc[1]*biw*0.5f + 0.5f) * static_cast<float>(fbo_h);
            pmin_x = std::min(pmin_x, bpx);
            pmax_x = std::max(pmax_x, bpx);
            pmin_y = std::min(pmin_y, bpy);
            pmax_y = std::max(pmax_y, bpy);
        }

        int min_px_x;
        int max_px_x;
        int min_px_y;
        int max_px_y;
        if (crosses_camera) {
            min_px_x = 0;
            max_px_x = fbo_w;
            min_px_y = 0;
            max_px_y = fbo_h;
        } else {
            min_px_x = static_cast<int>(std::floor(pmin_x)) - 1;
            max_px_x = static_cast<int>(std::ceil(pmax_x)) + 1;
            min_px_y = static_cast<int>(std::floor(pmin_y)) - 1;
            max_px_y = static_cast<int>(std::ceil(pmax_y)) + 1;
        }

        // 3. 转换成 tile 范围
        const int min_tc = std::max(0, min_px_x / kTileSize16);
        const int max_tc = std::min(grid_w - 1, max_px_x / kTileSize16);
        const int min_tr = std::max(0, min_px_y / kTileSize16);
        const int max_tr = std::min(grid_h - 1, max_px_y / kTileSize16);
        if (min_tc > max_tc || min_tr > max_tr) {
            continue;
        }

        // 4. 向覆盖的每个 tile 写入 light index（先到先得）
        for (int tr = min_tr; tr <= max_tr; ++tr) {
            for (int tc = min_tc; tc <= max_tc; ++tc) {
                const int t = tr * grid_w + tc;
                uint32_t& count = tile_counts[t];
                if (count >= static_cast<uint32_t>(kMaxLightsPerTile)) {
                    continue;
                }
                tiles[t].idx[count] = static_cast<uint8_t>(li);
                ++count;
            }
        }
    }

    // 5. 打包写入 tile 纹理（每 tile 4 个 texel，每 texel RGBA 各 1 个 uint8 index）
    std::vector<uint8_t> packed(static_cast<size_t>(tex_w) * tex_h * 4,
                                kLightIndexSentinel);
    for (int tr = 0; tr < grid_h; ++tr) {
        for (int tc = 0; tc < grid_w; ++tc) {
            const int t = tr * grid_w + tc;
            const Cell& cell = tiles[t];
            for (int k = 0; k < kTexelsPerTile; ++k) {
                const int gx = tc * kTexelsPerTile + k;
                uint8_t* px = &packed[(static_cast<size_t>(tr) * tex_w + gx) * 4];
                px[0] = cell.idx[k * 4 + 0];
                px[1] = cell.idx[k * 4 + 1];
                px[2] = cell.idx[k * 4 + 2];
                px[3] = cell.idx[k * 4 + 3];
            }
        }
    }

    glBindTexture(GL_TEXTURE_2D, tile_index_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex_w, tex_h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, packed.data());
    glBindTexture(GL_TEXTURE_2D, 0);
}

// ==================== DrawObject3D ====================

void Object3DRenderer::DrawObject3D(const Object3DCommand& cmd,
                                    const RenderCommandList& cmds,
                                    MeshManager& mesh_mgr,
                                    TextureManager& texture_mgr,
                                    ShaderManager& shader_mgr,
                                    const float mvp[16],
                                    unsigned int prog,
                                    unsigned int prog_full,
                                    unsigned int tile_index_tex) {
    const GPUMesh* mesh = mesh_mgr.GetMesh(cmd.mesh_id);
    CHECK(mesh != nullptr) << "DrawObject3D: mesh_id " << cmd.mesh_id
                           << " 未注册（DrawObject3D 前需先 RegisterMesh）";
    CHECK_GT(mesh->vao, 0u);

    const float up_len =
        std::sqrt(cmd.up.x()*cmd.up.x() + cmd.up.y()*cmd.up.y() + cmd.up.z()*cmd.up.z());
    const float fr_len =
        std::sqrt(cmd.front.x()*cmd.front.x() + cmd.front.y()*cmd.front.y() + cmd.front.z()*cmd.front.z());
    CHECK_GT(up_len, 1e-8f) << "DrawObject3D: up 向量不能为零";
    CHECK_GT(fr_len, 1e-8f) << "DrawObject3D: front 向量不能为零";

    float model[16];
    float mvp_final[16];
    BuildModelMatrix(cmd.center, cmd.up, cmd.front, model);
    Mat4Mul(mvp, model, mvp_final);

    const bool any_tex =
        (cmd.material.base_color_tex != 0) ||
        (cmd.material.has_metallic_tex && cmd.material.metallic_tex != 0) ||
        (cmd.material.has_roughness_tex && cmd.material.roughness_tex != 0) ||
        (cmd.material.emissive_tex != 0) ||
        (cmd.material.ao_tex != 0) ||
        (cmd.material.normal_tex != 0);
    const bool use_normal_map = (cmd.material.normal_tex != 0);

    CHECK(MeshHasFlag(mesh->flags, MeshVertexFlags::kNormal))
        << "DrawObject3D: mesh_id=" << cmd.mesh_id << " 需要 kNormal 属性";

    glPushAttrib(GL_ENABLE_BIT);

    unsigned int selected_prog;
    selected_prog = any_tex ? prog_full : prog;
    glUseProgram(selected_prog);

    glUniformMatrix4fv(glGetUniformLocation(selected_prog, "uMVP"),
                       1, GL_FALSE, mvp_final);
    glUniformMatrix4fv(glGetUniformLocation(selected_prog, "uModel"),
                       1, GL_FALSE, model);
    glUniform3f(glGetUniformLocation(selected_prog, "uBaseColor"),
                cmd.material.base_color.r, cmd.material.base_color.g,
                cmd.material.base_color.b);
    glUniform1f(glGetUniformLocation(selected_prog, "uMetallic"), cmd.material.metallic);
    glUniform1f(glGetUniformLocation(selected_prog, "uRoughness"), cmd.material.roughness);
    glUniform3f(glGetUniformLocation(selected_prog, "uEmissive"),
                cmd.material.emissive.r, cmd.material.emissive.g,
                cmd.material.emissive.b);
    glUniform1f(glGetUniformLocation(selected_prog, "uAO"), cmd.material.ao.r);
    glUniform3f(glGetUniformLocation(selected_prog, "uCameraPos"),
                cmds.camera.position.x(), cmds.camera.position.y(),
                cmds.camera.position.z());

    if (any_tex) {
        CHECK(MeshHasFlag(mesh->flags, MeshVertexFlags::kUV))
            << "DrawObject3D: 材质通道带纹理但 mesh 无 kUV 属性，无法纹理采样";
    }
    if (use_normal_map) {
        CHECK(MeshHasFlag(mesh->flags, MeshVertexFlags::kTangent))
            << "DrawObject3D: normal_tex 非 0 但 mesh 无 kTangent 属性，"
            << "无法构建 TBN（OBJ 加载器已自动推导 tangent）";
    }

    // ---- baseColor 纹理（TEXTURE1）----
    if (cmd.material.base_color_tex != 0) {
        unsigned int gl_tex = texture_mgr.GetGLTexture(cmd.material.base_color_tex);
        CHECK_NE(gl_tex, 0u)
            << "DrawObject3D: base_color_tex " << cmd.material.base_color_tex
            << " 未注册";
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, gl_tex);
        glUniform1i(glGetUniformLocation(selected_prog, "uBaseColorTex"), 1);
        glUniform1i(glGetUniformLocation(selected_prog, "uHasBaseColorTex"), 1);
    } else {
        glUniform1i(glGetUniformLocation(selected_prog, "uHasBaseColorTex"), 0);
    }

    // ---- metallic 纹理（TEXTURE2）----
    if (cmd.material.has_metallic_tex && cmd.material.metallic_tex != 0) {
        unsigned int gl_tex = texture_mgr.GetGLTexture(cmd.material.metallic_tex);
        CHECK_NE(gl_tex, 0u)
            << "DrawObject3D: metallic_tex " << cmd.material.metallic_tex
            << " 未注册";
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, gl_tex);
        glUniform1i(glGetUniformLocation(selected_prog, "uMetallicTex"), 2);
        glUniform1i(glGetUniformLocation(selected_prog, "uHasMetallicTex"), 1);
    } else {
        glUniform1i(glGetUniformLocation(selected_prog, "uHasMetallicTex"), 0);
    }

    // ---- roughness 纹理（TEXTURE3）----
    if (cmd.material.has_roughness_tex && cmd.material.roughness_tex != 0) {
        unsigned int gl_tex = texture_mgr.GetGLTexture(cmd.material.roughness_tex);
        CHECK_NE(gl_tex, 0u)
            << "DrawObject3D: roughness_tex " << cmd.material.roughness_tex
            << " 未注册";
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, gl_tex);
        glUniform1i(glGetUniformLocation(selected_prog, "uRoughnessTex"), 3);
        glUniform1i(glGetUniformLocation(selected_prog, "uHasRoughnessTex"), 1);
    } else {
        glUniform1i(glGetUniformLocation(selected_prog, "uHasRoughnessTex"), 0);
    }

    // ---- emissive 纹理（TEXTURE4）----
    if (cmd.material.emissive_tex != 0) {
        unsigned int gl_tex = texture_mgr.GetGLTexture(cmd.material.emissive_tex);
        CHECK_NE(gl_tex, 0u)
            << "DrawObject3D: emissive_tex " << cmd.material.emissive_tex
            << " 未注册";
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, gl_tex);
        glUniform1i(glGetUniformLocation(selected_prog, "uEmissiveTex"), 4);
        glUniform1i(glGetUniformLocation(selected_prog, "uHasEmissiveTex"), 1);
    } else {
        glUniform1i(glGetUniformLocation(selected_prog, "uHasEmissiveTex"), 0);
    }

    // ---- AO 纹理（TEXTURE5）----
    if (cmd.material.ao_tex != 0) {
        unsigned int gl_tex = texture_mgr.GetGLTexture(cmd.material.ao_tex);
        CHECK_NE(gl_tex, 0u)
            << "DrawObject3D: ao_tex " << cmd.material.ao_tex
            << " 未注册";
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, gl_tex);
        glUniform1i(glGetUniformLocation(selected_prog, "uAoTex"), 5);
        glUniform1i(glGetUniformLocation(selected_prog, "uHasAoTex"), 1);
    } else {
        glUniform1i(glGetUniformLocation(selected_prog, "uHasAoTex"), 0);
    }

    // ---- normal 纹理（TEXTURE6，法线映射 TBN）----
    if (cmd.material.normal_tex != 0) {
        unsigned int gl_tex = texture_mgr.GetGLTexture(cmd.material.normal_tex);
        CHECK_NE(gl_tex, 0u)
            << "DrawObject3D: normal_tex " << cmd.material.normal_tex
            << " 未注册";
        glActiveTexture(GL_TEXTURE6);
        glBindTexture(GL_TEXTURE_2D, gl_tex);
        glUniform1i(glGetUniformLocation(selected_prog, "uNormalTex"), 6);
        glUniform1i(glGetUniformLocation(selected_prog, "uHasNormalTex"), 1);
        glUniform1f(glGetUniformLocation(selected_prog, "uNormalScale"),
                    cmd.material.normal_scale);
    } else {
        glUniform1i(glGetUniformLocation(selected_prog, "uHasNormalTex"), 0);
        glUniform1f(glGetUniformLocation(selected_prog, "uNormalScale"), 1.0f);
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tile_index_tex);
    glUniform1i(glGetUniformLocation(selected_prog, "uTileLightIndices"), 0);

    glBindVertexArray(mesh->vao);
    if (mesh->index_count > 0) {
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(mesh->index_count),
                       GL_UNSIGNED_INT, nullptr);
    } else {
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(mesh->vertex_count));
    }
    glBindVertexArray(0);

    GLenum draw_err = glGetError();
    if (draw_err != GL_NO_ERROR) {
        LOG_FIRST_N(WARNING, 1) << "GL error after DrawObject3D: " << draw_err;
    }

    glPopAttrib();
}

// ==================== DrawObject3DShadow ====================

void Object3DRenderer::DrawObject3DShadow(const Object3DCommand& cmd,
                                          MeshManager& mesh_mgr,
                                          ShaderManager& shader_mgr,
                                          const float shadow_vp[16],
                                          unsigned int shadow_prog) {
    const GPUMesh* mesh = mesh_mgr.GetMesh(cmd.mesh_id);
    CHECK(mesh != nullptr) << "DrawObject3DShadow: mesh_id " << cmd.mesh_id
                           << " 未注册";
    CHECK_GT(mesh->vao, 0u);

    const float up_len =
        std::sqrt(cmd.up.x()*cmd.up.x() + cmd.up.y()*cmd.up.y() + cmd.up.z()*cmd.up.z());
    const float fr_len =
        std::sqrt(cmd.front.x()*cmd.front.x() + cmd.front.y()*cmd.front.y() + cmd.front.z()*cmd.front.z());
    CHECK_GT(up_len, 1e-8f) << "DrawObject3DShadow: up 向量不能为零";
    CHECK_GT(fr_len, 1e-8f) << "DrawObject3DShadow: front 向量不能为零";

    float model[16];
    float shadow_mvp[16];
    BuildModelMatrix(cmd.center, cmd.up, cmd.front, model);
    Mat4Mul(shadow_vp, model, shadow_mvp);

    glUseProgram(shadow_prog);
    glUniformMatrix4fv(glGetUniformLocation(shadow_prog, "uShadowMVP"),
                       1, GL_FALSE, shadow_mvp);

    glBindVertexArray(mesh->vao);
    if (mesh->index_count > 0) {
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(mesh->index_count),
                       GL_UNSIGNED_INT, nullptr);
    } else {
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(mesh->vertex_count));
    }
    glBindVertexArray(0);
}

// ==================== UploadSunData ====================

void Object3DRenderer::UploadSunData(
    ShaderManager& shader_mgr,
    unsigned int prog,
    unsigned int prog_full,
    const std::vector<CascadeFBO>& shadow_fbos,
    const float shadow_vp[][16],
    const ShadowConfig& cfg,
    const std::optional<DirectionalLight>& sun) {
    const int cascade_count = sun.has_value() ? cfg.cascade_count : 0;

    const unsigned int progs[2] = {prog, prog_full};
    for (int pidx = 0; pidx < 2; ++pidx) {
        unsigned int p = progs[pidx];
        glUseProgram(p);
        glUniform1i(shader_mgr.GetUniform(p, "uHasSun"), sun.has_value() ? 1 : 0);
        glUniform1i(shader_mgr.GetUniform(p, "uCascadeCount"), cascade_count);
        if (!sun.has_value()) {
            // 无太阳：仅置 uHasSun=0 / uCascadeCount=0（防止上一帧残留直射光）。
            continue;
        }
        glUniform3f(shader_mgr.GetUniform(p, "uSunDir"),
                    sun->direction.x(), sun->direction.y(), sun->direction.z());
        glUniform3f(shader_mgr.GetUniform(p, "uSunColor"),
                    sun->color.r, sun->color.g, sun->color.b);
        glUniform1f(shader_mgr.GetUniform(p, "uSunIntensity"), sun->intensity);

        // 各级联 far 距离 + 阴影淡出范围。
        CHECK_LE(cascade_count, ShadowConfig::kMaxCascades);
        glUniform1fv(shader_mgr.GetUniform(p, "uCascadeRanges"),
                     cascade_count, cfg.cascade_ranges);
        glUniform1f(shader_mgr.GetUniform(p, "uShadowFadeStart"), cfg.fade_start);
        glUniform1f(shader_mgr.GetUniform(p, "uShadowFadeEnd"), cfg.fade_end);

        // 绑各级联 shadow 深度纹理到 TEXTURE(7+i)，上传对应 ViewProj + texel。
        CHECK_EQ(shadow_fbos.size(), static_cast<size_t>(cascade_count))
            << "UploadSunData: shadow_fbos.size() 与 cascade_count 不一致";
        // 每级联独立深度偏置（ndc 单位，来自 ShadowConfig::cascade_bias）。
        // 上传全部 kMaxCascades 位（未用位取 0 或默认，shader 只索引实际级联）。
        static constexpr int kMaxC = jpov::ShadowConfig::kMaxCascades;
        float bias[kMaxC] = {0.004f, 0.004f, 0.004f, 0.004f, 0.004f};
        for (int c = 0; c < kMaxC; ++c) bias[c] = cfg.cascade_bias[c];
        glUniform1fv(shader_mgr.GetUniform(p, "uShadowBiasCascade"),
                     kMaxC, bias);
        for (int c = 0; c < cascade_count; ++c) {
            const unsigned int unit = 7u + static_cast<unsigned int>(c);
            glActiveTexture(GL_TEXTURE0 + unit);
            glBindTexture(GL_TEXTURE_2D, shadow_fbos[c].tex);
            // 纹理单元名 uShadowMap[i]（数组 sampler uniform）。
            std::string uni = "uShadowMap[" + std::to_string(c) + "]";
            glUniform1i(shader_mgr.GetUniform(p, uni.c_str()), static_cast<int>(unit));
            uni = "uShadowVP[" + std::to_string(c) + "]";
            glUniformMatrix4fv(shader_mgr.GetUniform(p, uni.c_str()),
                               1, GL_FALSE, shadow_vp[c]);
            uni = "uShadowTexel[" + std::to_string(c) + "]";
            glUniform1f(shader_mgr.GetUniform(p, uni.c_str()),
                        1.0f / static_cast<float>(shadow_fbos[c].size));
        }
        glActiveTexture(GL_TEXTURE0);
    }
}

}  // namespace jpov
