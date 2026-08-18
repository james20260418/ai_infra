// JPOV Gold Image Generator — 平放 wall 石材 + 单点光源
//
// 最小验证场景（作为逐步过渡到草地的第一步基线）：
//   - 场景里只有一块 wall 石板（scene_assets/wall_rock/wall_rock.gltf）
//     平放在地面上：其大面（4×3，带正常 rock_normal 法线贴图）朝上
//   - 单点光源 (0,3,0) 从正上方照下
//   - tile culling 开（默认 true），太阳（DirectionalLight sun）关（不设置）
//
// 目的：确认这块已知「法线贴图是好的」的 wall，在平放 + 顶光下能正常
// 呈现法线凹凸细节，作为随后过渡到草地 repeated test 的对照基线。
//
// 摆放约定（DrawGltfObject obj, center, up, front）：
//   wall 局部空间：X 宽 4、Y 高 3、Z 厚 0.2（±Z 是 4×3 大面，normal 在此）
//   - 局部 +Z → 世界 +Y（大面朝上）
//   - 局部 +Y → 世界 +Z（原本的高度躺倒沿世界 Z）
//   - 局部 +X → 世界 -X（right = cross(up, front) 推出，镜像无碍）
//   即 up={0,0,1}, front={0,1,0}，center.y=0.1 使 0.2 厚的板坐在地面。
//
// 输出: tools/jpov/test/object3d/repeated_wall_1280x720.png

#include <cstdio>
#include <string>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/gltf_object.h"
#include "tools/common/utils.h"

namespace {

std::string GetWallPath() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/tools/jpov/test/object3d/scene_assets/wall_rock/wall_rock.gltf";
        return p;
    }
    return jpov::GetProjectRoot() +
        "tools/jpov/test/object3d/scene_assets/wall_rock/wall_rock.gltf";
}

}  // namespace

class RepeatedWallGenerator : public JPOV {
public:
    using JPOV::JPOV;

    void SetGltfObject(jpov::GltfObject obj) { gltf_ = std::move(obj); }

    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        (void)input;
        (void)winfo;

        const float kResW = 1280.0f;
        const float kResH = 720.0f;
        cmds->camera.fbo_3d_width_  = kResW;
        cmds->camera.fbo_3d_height_ = kResH;

        // 相机 (3,3,3) up=+Y 看向原点，能同时看到平放 wall 的顶面与侧面
        cmds->camera.position = {3.5f, 3.0f, 3.5f};
        cmds->camera.target   = {0.0f, 0.1f, 0.0f};
        cmds->camera.up       = {0.0f, 1.0f, 0.0f};

        // 单点光源从正上方 (0,3,0)，让朝上的大面法线细节受光清晰
        cmds->point_lights.push_back({
            {0.0f, 3.0f, 0.0f},          // position
            {1.0f, 1.0f, 1.0f, 1.0f},   // color
            12.0f,                       // intensity
            0.5f                         // physical_radius
        });

        // 平放 wall：大面朝上（顶光正好照在大面上）。
        // 注：glTF loader 做了 z-up→y-up 坐标转换（jpov=Vec3f(gx,-gz,gy)），
        // wall 大面(4×3)在 jpov 局部已是 XZ 平面、法线=局部+Y（厚 0.2）。
        // 故 up/front 用恒等朝向 {0,1,0},{0,0,1}，墙即平躺地上。
        const jpov::Vec3f up    = {0.0f, 1.0f, 0.0f};   // 局部 +Y（法线）→ 世界 +Y
        const jpov::Vec3f front = {0.0f, 0.0f, 1.0f};   // 局部 +Z → 世界 +Z
        cmds->DrawGltfObject(gltf_, {0.0f, 0.1f, 0.0f}, up, front);
    }

private:
    jpov::GltfObject gltf_;
};

int main() {
    const std::string outpath =
        "/james_pm/ai_infra_2/tools/jpov/test/object3d/repeated_wall_1280x720.png";

    JPOV::Config cfg;
    cfg.title = "Repeated Wall (flat on ground, single top light) Gold Generator";
    cfg.headless = true;
    RepeatedWallGenerator app(cfg);
    app.Init();

    jpov::GltfObject gltf = app.LoadGltf(GetWallPath());
    CHECK(!gltf.empty()) << "LoadGltf wall failed / empty";
    LOG(INFO) << "Loaded wall: " << gltf.size() << " primitive(s)";
    app.SetGltfObject(std::move(gltf));

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    LOG(INFO) << "Repeated wall gold image generated: " << outpath;
    return 0;
}
