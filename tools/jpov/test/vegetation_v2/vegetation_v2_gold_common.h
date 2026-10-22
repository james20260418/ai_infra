// vegetation v2 查看器 gold 场景 —— test 与 generator 共用的同一份构建代码。
//
// 为什么要共用：万一 test 自己搭场景，改查看器时 test 还是绿的，等于没保护。
// 这里直接驱动查看器的 app（vegetation_v2_app.h），路径与
// `build_jpov_vegetation_v2_viewer.sh` 打印的「完整组合」命令一致。
//
// **两张 gold，各自保护不同的东西**（llvmpipe 的脾气决定的，见下）：
//
//   cards_1280x720.png  ← debug 绿卡视图（不贴叶图）：
//       卡是**不透明纯色盒子**，渲染逐字节确定（实测连续 6 次 diff = 0）。
//       它精确锁住「叶卡到底摆在哪儿」：anchor / up / normal / width / height
//       以及 anchor_mode → UV 的推导。改错半张卡在这张图上就是明显差异。
//
//   leaf_1280x720.png   ← 贴叶图（--leaf-black 30）+ 树皮：
//       叶卡是 alpha cutout + 覆盖率 mip，llvmpipe 下**不是逐字节确定**的
//       （实测同一二进制多次运行，8×8 ROI 均值差 0 ~ 27 之间跳），所以这张只能
//       用宽阈值当「烟雾报警」：叶子整片消失 64、树皮掉了 46 会被抓住，
//       细粒度的错位交给上面那张 cards gold。
//
// 机位是**近景**（R=1.6，cam_target_y=3）：远景时叶子只占画面 ~2%，连「叶卡横移
// 半张卡」都只有 3.7 的均值差，压不住噪声；近景能放大到 16.9。

#ifndef JPOV_TOOLS_VEGETATION_TOOL_V2_GOLD_COMMON_H_
#define JPOV_TOOLS_VEGETATION_TOOL_V2_GOLD_COMMON_H_

#include <cstdlib>
#include <string>

#include <glog/logging.h>

#include "tools/jpov/test/test_utils.h"
#include "tools/jpov/tools/vegetation_tool_v2/viewer/vegetation_v2_app.h"

namespace vegv2 {
namespace goldtest {

// 样例资产目录（shape + leaf + bark 都在这里）：
//   bazel test: $TEST_SRCDIR/__main__/tools/jpov/tools/vegetation_tool_v2
//   本地 bazel run: <repo>/tools/jpov/tools/vegetation_tool_v2
inline std::string SampleAssetDir() {
  const char* srcdir = std::getenv("TEST_SRCDIR");
  if (srcdir != nullptr) {
    std::string p = srcdir;
    if (!p.empty() && p.back() != '/') {
      p.push_back('/');
    }
    return p + "__main__/tools/jpov/tools/vegetation_tool_v2";
  }
  return jpov::GetProjectRoot() + "tools/jpov/tools/vegetation_tool_v2";
}

// 出图分辨率（与查看器默认一致）。
constexpr int kWidth = 1280;
constexpr int kHeight = 720;

// 出图文件（相对 gold 目录 / output 目录）。
constexpr const char* kCardsGold = "vegetation_v2_cards_1280x720.png";
constexpr const char* kLeafGold = "vegetation_v2_leaf_1280x720.png";

// 两张 gold 共用的场景参数。with_leaf=false → debug 绿卡视图。
inline CliArgs GoldenArgs(bool with_leaf) {
  const std::string dir = SampleAssetDir();
  CliArgs a;
  a.shape_path = dir + "/samples/willow.pbtxt";
  if (with_leaf) {
    a.leaf_path = dir + "/assets/leaf/willow_leaf.png";
    // 这批样例叶卡背景是 ≈10~13 的灰噪、内容 ≥44 ⇒ 软过档 [0.5t, 1.5t] 取 t=30 得 [15,45]。
    a.leaf_black = 30.0f;
  }
  a.bark_dir = dir + "/assets/bark/bark001";
  a.phi_deg = 12.0;       // 与 viewer --capture 的 front 机位一致
  a.theta_deg = 0.0;      // 正对 -Z（front）
  a.radius = 1.6;         // 近景：叶卡占画面一成以上，错位才看得出来
  a.cam_target_y = 3.0;
  // capture=true 让 BuildScene 走「headless 不画 UI 面板」分支（面板要 CJK 字体图集）。
  a.capture = true;
  a.show_panel = false;
  a.hide_hud = true;      // HUD 是文字，会跟着字体/相机字符串变，别进基准图
  return a;
}

// 跑一遍 gold 场景，把图写到 out_png。返回 false = 场景加载失败。
//
// 前 3 帧是热身（字体/材质/纹理上传），与 viewer 的 capture 路径一致，
// 不热身会拍到还没上传完的贴图。
inline bool RenderGolden(bool with_leaf, const std::string& out_png) {
  const CliArgs args = GoldenArgs(with_leaf);

  JPOV::Config cfg;
  cfg.title = "JPOV vegetation v2 gold";
  cfg.width = static_cast<float>(kWidth);
  cfg.height = static_cast<float>(kHeight);
  cfg.headless = true;
  // 不出 UI、不画叶卡标签、不画 HUD ⇒ 不需要字体图集（CI sandbox 里没有 fonts/）。
  cfg.fonts = {};

  VegetationV2App app(cfg);
  app.Init();
  if (!LoadShape(args.shape_path, &app.shape_)) {
    LOG(ERROR) << "读不到 shape: " << args.shape_path;
    return false;
  }
  app.shape_path_ = args.shape_path;
  app.show_panel_ = false;
  app.show_labels_ = false;
  app.show_hud_ = false;
  app.BuildScene(args);

  jpov::WindowInfo winfo;
  winfo.width = static_cast<float>(kWidth);
  winfo.height = static_cast<float>(kHeight);
  jpov::InputSnapshot input{};
  for (int i = 0; i < 3; ++i) {
    app.RunOnce(input, winfo, "/tmp/_veg_v2_gold_warm.png");
  }
  app.RunOnce(input, winfo, out_png.c_str());
  app.Finalize();
  LOG(INFO) << "gold 场景已渲染（" << (with_leaf ? "贴叶图" : "debug 绿卡") << "）: " << out_png;
  return true;
}

}  // namespace goldtest
}  // namespace vegv2

#endif  // JPOV_TOOLS_VEGETATION_TOOL_V2_GOLD_COMMON_H_
