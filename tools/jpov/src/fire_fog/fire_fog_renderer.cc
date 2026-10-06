// JPOV FireFogRenderer 成员函数实现
//
// 见 fire_fog_renderer.h。本 PR 只搭框架：Init/Finalize/Draw 为骨架，
// 真正的 pass 见 tools/jpov/docs/jpov_fire_fog_design.md §10.6（M1a）。

#include "tools/jpov/src/fire_fog/fire_fog_renderer.h"

#include <glog/logging.h>

namespace jpov {

FireFogRenderer::~FireFogRenderer() {
    Finalize();
}

void FireFogRenderer::Init(ShaderManager* shader_mgr) {
    CHECK(shader_mgr != nullptr);
    shader_mgr_ = shader_mgr;
    // M1a：编译雾火 shader、建 tile 剪枝资源（tile 索引表 / 团属性缓冲纹理）。
}

void FireFogRenderer::Finalize() {
    // M1a：释放 Init 申请的 GPU 资源。当前无自有 GL 资源，仅复位状态。
    bodies_.clear();
    shader_mgr_ = nullptr;
}

void FireFogRenderer::Draw(const std::vector<PointFog>& /*fogs*/,
                           const Camera& /*cam*/,
                           const float /*view_proj*/[16],
                           int /*viewport_w*/,
                           int /*viewport_h*/,
                           unsigned int /*scene_depth_tex*/) {
    // v1 骨架：pass 尚未实现（空操作，不影响画面）。
    // M1a：fogs → FogBody → 屏幕 tile 剪枝 → 逐像素 ZDist 累加 → 末端积分 → 就地合成。
    // 见 tools/jpov/docs/jpov_fire_fog_design.md §10.6。
}

}  // namespace jpov
