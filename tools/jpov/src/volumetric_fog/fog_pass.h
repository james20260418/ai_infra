// JPOV 局部体积雾 — 全屏 fog pass（GL 侧）
//
// 见 docs/jpov_volumetric_fog_design.md §3.4 / §8。在 3D 不透明 pass 的 resolve 之后、
// highlight / bloom / tone map 之前运行：读 HDR 颜色 + 场景深度 + tile 索引表 + 雾体属性，
// 一次全屏三角形把雾就地合成进 HDR，输出到一张 ping-pong 纹理。

#ifndef JPOV_SRC_VOLUMETRIC_FOG_FOG_PASS_H_
#define JPOV_SRC_VOLUMETRIC_FOG_FOG_PASS_H_

#include "geom/common/vec.h"
#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/src/shader_manager.h"

namespace jpov {
namespace volumetric_fog {

// fog pass 持有的 GL 资源（随渲染尺寸/雾体数按需重建）。
struct FogPassState {
    unsigned int out_fbo = 0;      // 输出 FBO（RGBA16F）
    unsigned int out_tex = 0;      // 输出颜色纹理
    int w = 0;
    int h = 0;
    unsigned int tile_tex = 0;     // tile 索引（R16UI）
    int tile_tex_w = 0;
    int tile_tex_h = 0;
    unsigned int attr_tex = 0;     // 雾体属性（RGBA32F，宽 = kFogAttrTexels）
    int attr_h = 0;
};

// 执行 fog pass，返回「已合成雾」的 HDR 颜色纹理。
//
//   cmds            : 含 fog_spheres / fog_cylinders / sun / ambient / point_lights
//   mvp             : 相机 MVP（Proj*View，列主序）
//   cam_pos         : 相机世界位置
//   hdr_input_tex   : 场景 HDR 颜色（RGBA16F，采样用输入）
//   scene_depth_tex : 场景深度（R32F，gl_FragCoord.z）
//   fbo_w / fbo_h   : 渲染分辨率
//
// 前置：cmds.fog_spheres/cylinders 非空（空则调用方不应进入本函数）。
// 返回 0 表示未执行（无雾体）。
unsigned int RunFogPass(const RenderCommandList& cmds, ShaderManager& shader_mgr,
                        const float mvp[16], const Vec3f& cam_pos,
                        unsigned int hdr_input_tex, unsigned int scene_depth_tex,
                        int fbo_w, int fbo_h, FogPassState* state);

// 释放 fog pass 的 GL 资源。
void DestroyFogPass(FogPassState* state);

}  // namespace volumetric_fog
}  // namespace jpov

#endif  // JPOV_SRC_VOLUMETRIC_FOG_FOG_PASS_H_
