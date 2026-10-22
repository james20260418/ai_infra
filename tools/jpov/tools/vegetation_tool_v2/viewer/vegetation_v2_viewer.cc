// 植被工具 v2 —— 只做一件事：把 ShapeProto（pb.txt）渲染出来看形状对不对。
//
// 刻意【不接材质】：到这一阶段要判断的是**拓扑/UV 尺度/分叉**，贴图会干扰判断。
//   * 树干：按格式定义逐枝扫管，**每根枝单独一个 mesh**（便于逐枝上色/逐枝隐藏）
//   * 层级配色：level 0 深棕 → 越深越浅（一眼看出父子关系）
//   * 叶卡：用**盒子**表示（不是薄片）—— 盒子能看清朝向与尺寸，薄片侧面看不见
//   * 类型配色：最多 4 种绿，按 type 取模
//
// 交互：K 切换叶卡、[ / ] 调最大显示层级、P 打印相机、H 打印叶卡帮助。
//
// 用法：
//   jpov_vegetation_v2_viewer                      # 只渲染默认范本的拓扑
//   jpov_vegetation_v2_viewer --shape /tmp/willow.pbtxt
//   jpov_vegetation_v2_viewer --shape ... --capture /tmp/shots --cam_phi 10 --cam_r 4
//   --normal-scale <f>   法线强度（默认 1.5；0~3）
//   --uv-vscale 0.5      v 方向缩放（默认 0 = 自动取 albedo 宽/高）
//   --leaf leaf.png      叶卡贴图（整图一格；近乎黑的像素当透明）
//   --leaf2 leaf_flip.png  leaf type 1（镜像手性）的素材；缺省回退 --leaf
//   --curl 0.5           顶点法线球面化强度 A（0 = 平面法线）
//   --leaf-normal 1.0    由 alpha 生成的法线贴图强度 B（0 = 关）
//   --cam-target-y 3.0   注视点高度（配合 --cam_r 0.9 看叶卡细节）

//
// 本文件只留 CLI 壳：解析参数 → new VegetationV2App → 交互跑 / headless 出图。
// 场景与渲染全在 vegetation_v2_app.h（gold test 复用同一份代码）。

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/tools/vegetation_tool_v2/viewer/vegetation_v2_app.h"

namespace vegv2 {
namespace {

CliArgs ParseCli(int argc, char** argv) {
  CliArgs a;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_val = (i + 1 < argc);
    if (arg == "--bark" && has_val) {
      a.bark_dir = argv[++i];
    } else if (arg == "--shape" && has_val) {
      a.shape_path = argv[++i];
    } else if (arg == "--capture") {
      a.capture = true;
      if (has_val && argv[i + 1][0] != '-') {
        a.out_dir = argv[++i];
      }
    } else if (arg == "--cam_phi" && has_val) {
      a.phi_deg = std::atof(argv[++i]);
    } else if (arg == "--cam_theta" && has_val) {
      a.theta_deg = std::atof(argv[++i]);
    } else if (arg == "--cam_r" && has_val) {
      a.radius = std::atof(argv[++i]);
      CHECK_GT(a.radius, 0.0) << "--cam_r 必须 > 0";
    } else if (arg == "--leaf" && has_val) {
      a.leaf_path = argv[++i];
    } else if (arg == "--leaf2" && has_val) {
      a.leaf2_path = argv[++i];
    } else if (arg == "--leaf-black" && has_val) {
      a.leaf_black = static_cast<float>(std::atof(argv[++i]));
      CHECK_GE(a.leaf_black, 0.0f) << "--leaf-black 不能为负";
    } else if (arg == "--curl" && has_val) {
      a.leaf_curl = static_cast<float>(std::atof(argv[++i]));
      CHECK_GE(a.leaf_curl, 0.0f) << "--curl 不能为负";
    } else if (arg == "--leaf-normal" && has_val) {
      a.leaf_normal = static_cast<float>(std::atof(argv[++i]));
      CHECK_GE(a.leaf_normal, 0.0f) << "--leaf-normal 不能为负（0 = 关掉法线贴图）";
    } else if (arg == "--normal-scale" && has_val) {
      a.normal_scale = static_cast<float>(std::atof(argv[++i]));
      CHECK_GT(a.normal_scale, 0.0f) << "--normal-scale 必须 > 0";
    } else if (arg == "--uv-vscale" && has_val) {
      a.uv_vscale = static_cast<float>(std::atof(argv[++i]));
      CHECK_GT(a.uv_vscale, 0.0f) << "--uv-vscale 必须 > 0";
    } else if (arg == "--show-panel") {
      a.show_panel = true;
    } else if (arg == "--only-type" && has_val) {
      a.only_type = std::atoi(argv[++i]);
      CHECK_GE(a.only_type, 0) << "--only-type 不能为负";
    } else if (arg == "--no-leaf") {
      a.hide_leaf = true;
    } else if (arg == "--no-hud") {
      a.hide_hud = true;
    } else if (arg == "--no-bark") {
      a.hide_bark = true;
    } else if (arg == "--cam-target-y" && has_val) {
      a.cam_target_y = static_cast<float>(std::atof(argv[++i]));
    } else if (arg == "--ground-y" && has_val) {
      a.ground_y = static_cast<float>(std::atof(argv[++i]));
    } else if (arg == "--max-level" && has_val) {
      a.max_level = std::atoi(argv[++i]);
      CHECK_GE(a.max_level, 0) << "--max-level 不能为负";
    } else if (arg.rfind("--", 0) == 0) {
      LOG(WARNING) << "未知参数: " << arg << "（已忽略）";
    }
  }
  // shape 有默认值（打包目录里的 shapes/willow.pbtxt），叶卡/树皮默认**空**——
  // 不给就只渲染拓扑（树干棕色 + debug 叶卡），这是「组合查看器」的底座：
  // 用户提供 shape / bark / leaf 三样，给几样渲染几样。
  if (a.shape_path.empty()) {
    a.shape_path = "shapes/willow.pbtxt";
    LOG(INFO) << "未给 --shape，用默认范本: " << a.shape_path;
  }
  if (!std::ifstream(a.shape_path).good()) {
    LOG(FATAL) << "读不到 shape: " << a.shape_path
               << "\n用法: jpov_vegetation_v2_viewer [--shape <path.pbtxt>]"
               << " [--bark <dir>] [--leaf <png>] [--leaf2 <png>]"
               << "\n  --shape 省略时用 shapes/willow.pbtxt（打包目录内）"
               << "\n  --bark / --leaf 省略 = 只渲染 shape（不贴树皮/叶图）";
  }
  return a;
}

}  // namespace
}  // namespace vegv2

using namespace vegv2;



int main(int argc, char** argv) {
  const CliArgs args = ParseCli(argc, argv);

  JPOV::Config cfg;
  cfg.title = "JPOV - vegetation v2 viewer";
  cfg.width = kViewerWidth;
  cfg.height = kViewerHeight;
  cfg.headless = args.capture;
  // 字体只用来画 HUD 和 debug 标签。缺字体不该让整个查看器崩掉（CI / 无字体环境）：
  // 有就注册，没有就降级成「不画文字」，由 app 自己把 HUD/标签关掉。
  const JPOV::FontEntry kWantFonts[] = {
      {"fonts/NotoSansCJK-Regular.ttc", 0, jpov::kFontBuiltinCJK},
      {"fonts/DejaVuSans.ttf", 0, jpov::kFontBuiltinLatin},
  };
  for (const JPOV::FontEntry& f : kWantFonts) {
    if (std::ifstream(f.path).good()) {
      cfg.fonts.push_back(f);
    } else {
      LOG(WARNING) << "缺字体（跳过，不画文字）: " << f.path;
    }
  }

  VegetationV2App app(cfg);
  app.has_fonts_ = !cfg.fonts.empty();
  app.Init();
  if (!LoadShape(args.shape_path, &app.shape_)) {
    return 1;
  }
  app.shape_path_ = args.shape_path;
  app.PrintLeafTypeHelp(args.shape_path);
  app.BuildScene(args);

  if (!args.capture) {
    app.Run();
    app.Finalize();
    return 0;
  }

  // headless：先热身几帧（字体/材质上传），再按三个角度各拍一张
  jpov::WindowInfo winfo;
  winfo.width = kViewerWidth;
  winfo.height = kViewerHeight;
  jpov::InputSnapshot input{};
  for (int i = 0; i < 3; ++i) {
    app.RunOnce(input, winfo, "/tmp/_veg_v2_warm.png");
  }
  const std::string dir = args.out_dir.empty() ? "/tmp" : args.out_dir;
  const std::pair<const char*, double> shots[] = {
      {"front", 0.0}, {"three_quarter", 45.0}, {"side", 90.0}};
  for (const auto& s : shots) {
    app.view_.theta = s.second * 3.14159265358979323846 / 180.0;
    const std::string path = dir + "/veg_v2_" + s.first + ".png";
    LOG(INFO) << "[camera] " << app.CamString();
    app.RunOnce(input, winfo, path.c_str());
    LOG(INFO) << "capture: " << path;
  }
  app.Finalize();
  return 0;
}
