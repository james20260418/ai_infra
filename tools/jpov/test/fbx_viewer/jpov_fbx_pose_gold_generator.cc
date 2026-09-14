// JPOV FBX 观察器 gold generator —— 写仓库 gold image
//
// 与 test（jpov_fbx_pose_gold_test）共用 jpov_fbx_pose_gold_common.h 场景：
// 直接用观察器本体（FbxViewerApp，headless + 不画面板）渲出 kGoldTimeSeconds 那一帧，
// 写入仓库 gold PNG。gold 内容 = 用户跑起来看到的画面，不存在"另写一份场景"的分叉。
#include <string>

#include "glog/logging.h"

#include "tools/jpov/test/fbx_viewer/jpov_fbx_pose_gold_common.h"

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);

    const std::string outpath =
        jpov::GetTestDataDir() + jpov_fbx_pose_gold::GetGoldRelPath();

    JPOV::Config cfg = jpov_fbx_pose_gold::MakeConfig("JPOV FBX Pose Gold");
    jpov_fbx_viewer::FbxViewerApp app(cfg);
    jpov_fbx_pose_gold::SetupApp(&app, jpov_fbx_pose_gold::kGoldTimeSeconds,
                                 /*rest_pose*/ false);
    jpov_fbx_pose_gold::RenderFrame(&app, outpath.c_str());
    app.Finalize();

    LOG(INFO) << "fbx pose gold generated: " << outpath;
    return 0;
}
