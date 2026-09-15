// JPOV FBX 观察器 gold generator —— 写仓库 gold image（两张）
//
// 与 test（jpov_fbx_pose_gold_test）共用 jpov_fbx_pose_gold_common.h 场景：
//   ① 直接用观察器本体（FbxViewerApp，headless + 不画面板）渲出 kGoldTimeSeconds 那一帧
//      （仅 fbx 源骨人，红）→ 基础 gold；
//   ② 再传 glb 渲「对照组」（两者并列：红=fbx 源 / 蓝=glb rest 被同一份 pose 数值直搬）
//      → 第二张 gold。两张都走同一条 OneIteration，不存在“另写一份场景”的分叉。
#include <string>

#include "glog/logging.h"

#include "tools/jpov/test/fbx_viewer/jpov_fbx_pose_gold_common.h"

namespace {

// 渲一张 gold（走共用的 MakeApp，保证与 test 零分叉）。
void Generate(const std::string& outpath, const std::string& glb_path,
              jpov_fbx_viewer::ViewMode view_mode, const char* title) {
    std::unique_ptr<jpov_fbx_viewer::FbxViewerApp> app =
        jpov_fbx_pose_gold::MakeApp(title, jpov_fbx_pose_gold::kGoldTimeSeconds,
                                    /*rest_pose*/ false, glb_path, view_mode);
    jpov_fbx_pose_gold::RenderFrame(app.get(), outpath.c_str());
    LOG(INFO) << "gold generated: " << outpath;
}

}  // namespace

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);
    const std::string dir = jpov::GetTestDataDir();

    // ① 基础 gold（仅 fbx 源骨人）
    Generate(dir + jpov_fbx_pose_gold::GetGoldRelPath(), /*glb_path*/ "",
             jpov_fbx_viewer::ViewMode::kFbxOnly, "JPOV FBX Pose Gold");

    // ② 对照组 gold（fbx + glb 并列，蓝骨 = 数值直搬的“无重定向”对照）
    Generate(dir + jpov_fbx_pose_gold::GetGlbNaiveGoldRelPath(),
             jpov_fbx_pose_gold::GlbPath(), jpov_fbx_viewer::ViewMode::kBothNoRetarget,
             "JPOV FBX Pose + glb naive Gold");
    return 0;
}
