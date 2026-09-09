// JPOV 骨架蒙皮 T-rest gold test —— 验证蒙皮渲染链路跑通（M1）
//
// 与 jpov_skeleton_gold_generator 共用同一场景(蓝人 bind pose T-rest + sunny-day)。
// 测试通过条件：渲染链路跑通 + 输出非平凡效果图 + 库内 gold 存在（generator 产出）。
// PBR/llvmpipe 三稳态非确定（同 object3d PBR gold 决策），故不做逐像素颜色比对；
// 效果正确性由 leader/Danis 肉眼查看 generator 产出的
// human_skeleton_gold_t_rest_1280x720.png 判断。
#include <cstdio>
#include <string>

#include <glog/logging.h>

#include "third_party/stb/stb_image.h"

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/src/gltf_loader.h"
#include "tools/jpov/src/skeleton/skinning_bind_pose.h"
#include "tools/jpov/test/test_utils.h"

namespace {

std::string GetOutputDir() {
    return jpov::GetOutputDir() + "jpov_skeleton_gold_test/";
}

std::string GetGoldPath() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/tools/jpov/test/skeleton/human_skeleton_gold_t_rest_1280x720.png";
        return p;
    }
    return jpov::GetProjectRoot() +
        "tools/jpov/test/skeleton/human_skeleton_gold_t_rest_1280x720.png";
}

}  // namespace

int main() {
    std::string outdir = GetOutputDir();
    std::system(("mkdir -p " + outdir).c_str());
    const std::string outpath = outdir + "rendered.png";

    // 渲染一帧（与 generator 相同场景/光）到 temp。
    const std::string glb =
        jpov::GetProjectRoot() +
        "tools/jpov/test/object3d/mixamo_male/mixamo_male.glb";

    JPOV::Config cfg;
    cfg.title = "JPOV Skeleton Gold Test (T-rest)";
    cfg.headless = true;

    // 具体 app：OneIteration 里推一条蒙皮指令（与 generator 同一场景/光照）。
    struct TestApp : public JPOV {
        using JPOV::JPOV;
        uint32_t mesh_id_ = 0, skel_id_ = 0;
        jpov::PBRMaterial mat_;
        void Set(uint32_t m, uint32_t s, jpov::PBRMaterial mat) {
            mesh_id_ = m; skel_id_ = s; mat_ = std::move(mat);
        }
        void OneIteration(int64_t, const jpov::InputSnapshot&,
                          const jpov::WindowInfo&, jpov::RenderCommandList* cmds) override {
            const float kResW = 1280.0f, kResH = 720.0f;
            cmds->camera.fbo_3d_width_ = kResW;
            cmds->camera.fbo_3d_height_ = kResH;
            const jpov::Vec3f target{0.0f, 1.0f, 0.0f};
            cmds->camera.position = {0.0f, 1.6f, 4.2f};
            cmds->camera.target = target;
            cmds->camera.near = 0.05f;
            cmds->sun = jpov::DirectionalLight{{0.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, 3.0f};
            cmds->ambient = jpov::AmbientLight{.color = {1, 1, 1, 1}, .intensity = 0.3f};
            jpov::SkinnedInstanceState inst;
            inst.center = {0.0f, 0.0f, 0.0f};
            inst.up = {0.0f, 1.0f, 0.0f};
            inst.front = {0.0f, 0.0f, 1.0f};
            inst.scale = 1.0f;
            inst.pose_a = 0; inst.pose_b = 0; inst.ratio = 0.0f;
            std::vector<jpov::SkinnedInstanceState> instances{inst};
            cmds->DrawMeshWithSkeleton(mesh_id_, skel_id_, mat_, std::move(instances));
        }
    };

    TestApp app(cfg);
    app.Init();

    jpov::GltfObject gltf = app.LoadGltf(glb);
    CHECK(!gltf.primitives.empty()) << "mixamo_male.glb 应含蒙皮 primitive";

    std::vector<jpov::SkeletonType> skels;
    CHECK(jpov::LoadGltfSkeleton(glb, &skels));
    CHECK(!skels.empty());
    std::vector<jpov::SkeletonPose> poses;
    poses.push_back(jpov::MakeMixamoBindPose());
    const uint32_t skel_id = app.RegisterSkeleton(skels[0], poses);
    const uint32_t mesh_id = gltf.primitives[0].mesh_id;
    jpov::PBRMaterial material = gltf.primitives[0].material;
    app.Set(mesh_id, skel_id, std::move(material));

    // 校验库内 gold image 存在（generator 生成；缺失=回归）。
    {
        const std::string gold = GetGoldPath();
        FILE* f = std::fopen(gold.c_str(), "rb");
        CHECK(f != nullptr) << "gold image 缺失，请先跑 "
            "jpov_skeleton_gold_generator: " << gold;
        std::fclose(f);
        LOG(INFO) << "gold image 存在: " << gold;
    }

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    // 渲染输出非平凡：确认输出 PNG 非全黑。
    int w = 0, h = 0, ch = 0;
    unsigned char* px = stbi_load(outpath.c_str(), &w, &h, &ch, 0);
    CHECK(px != nullptr) << "渲染输出应可读取: " << outpath;
    bool non_black = false;
    const size_t n = static_cast<size_t>(w) * h * ch;
    for (size_t i = 0; i < n; ++i) {
        if (px[i] > 8) { non_black = true; break; }
    }
    stbi_image_free(px);
    CHECK(non_black) << "渲染输出全黑/近黑，蒙皮链路可能没画出内容: " << outpath;

    LOG(INFO) << "jpov_skeleton_gold_test (T-rest) PASSED";
    return 0;
}
