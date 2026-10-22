// 植被工具 v2 —— **植物组合查看器**的命令行壳（只有参数解析 + `RunViewer`）。
//
// 定位：把 shape（拓扑）+ bark（树皮材质）+ leaf（叶卡素材）组合成植物渲染出来，
// 给人和 AI 做资产验收；**三样给几样就渲染几样**。
// 场景/渲染/交互的实现全在 vegetation_v2_app.cc（只编译一次，gold test 用的是同一个二进制）。
//
// 交互：左键拖 = 转视角、滚轮 = 推拉、中键上下拖 = 平移注视点；
//       K 切叶卡/绿卡、[ ] 调最大层级、P 打印相机、H 打印叶卡帮助。
//
// 用法（详细说明见 ../README.md）：
//   jpov_vegetation_v2_viewer                          # 默认范本 + 只渲染拓扑
//   jpov_vegetation_v2_viewer --leaf leaf/willow_leaf.png --leaf-black 30 --bark bark/bark001
//   jpov_vegetation_v2_viewer --capture shots/full --leaf ... --bark ...   # 无窗口出三张图
//
//   --shape <pbtxt>      拓扑（省略 = shapes/willow.pbtxt）
//   --bark <dir>         树皮材质目录（省略 = 树干按 level 上棕色）
//   --leaf/--leaf2 <png> 叶卡素材；leaf2 给镜像手性的 leaf type 1（省略回退 leaf）
//   --leaf-black <f>     黑底素材阈值，软过档 [0.5t, 1.5t]（样例素材给 30）
//   --curl / --leaf-normal / --normal-scale / --uv-vscale   法线与 UV 相关强度
//   --cam_phi/--cam_theta/--cam_r / --cam-target-y           视角（度/度/米/米）
//   --max-level <n> / --only-type <n>                        只画前 n 层 / 只画某类叶卡
//   --no-leaf / --no-bark / --no-hud                         关叶图（看 debug 绿卡）/ 关树皮 / 关 HUD
//   --show-panel                                              capture 时也画 UI 面板
//   --capture <dir>                                          无窗口出图（front/three_quarter/side）
//   --bake <out.glb>                                         烘焙一份 glb 后退出（CI 用）
//   --glb <in.glb>                                           启动即进 glb 展示态（带风调试面板）
//                                                            **可以不给 --shape**（纯 glb 查看模式，
//                                                            此时无原材料态：不能烘焙/回退）
//   --no-wind / --wind-time <秒>                              关风 / 钉住风时间（A/B、出图可复现）

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
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
    } else if (arg == "--bake" && has_val) {
      a.bake_glb_path = argv[++i];
    } else if (arg == "--glb" && has_val) {
      a.glb_path = argv[++i];
    } else if (arg == "--no-wind") {
      a.wind_off = true;
    } else if (arg == "--wind-time" && has_val) {
      a.wind_time = static_cast<float>(std::atof(argv[++i]));
    } else if (arg == "--wind-amp" && has_val) {
      a.wind_amp = static_cast<float>(std::atof(argv[++i]));
    } else if (arg == "--wind-turb" && has_val) {
      a.wind_turb = static_cast<float>(std::atof(argv[++i]));
    } else if (arg == "--wind-blow" && has_val) {
      a.wind_blow = static_cast<float>(std::atof(argv[++i]));
    } else if (arg == "--wind-blow-exp" && has_val) {
      a.wind_blow_exp = static_cast<float>(std::atof(argv[++i]));
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
  // **纯 glb 查看模式**：给了 --glb 就不强求 shape（"别人发我一个 glb，我只想看看"）。
  // 找不到 shape 时只警告，由 RunViewer 决定走纯 glb 路径；没给 --glb 才 FATAL。
  if (!std::ifstream(a.shape_path).good()) {
    if (a.glb_path.empty()) {
      LOG(FATAL) << "读不到 shape: " << a.shape_path
                 << "\n用法: jpov_vegetation_v2_viewer [--shape <path.pbtxt>]"
                 << " [--bark <dir>] [--leaf <png>] [--leaf2 <png>]"
                 << "\n    或只看一个 glb: jpov_vegetation_v2_viewer --glb <path.glb>"
                 << "\n  --shape 省略时用 shapes/willow.pbtxt（打包目录内）"
                 << "\n  --bark / --leaf 省略 = 只渲染 shape（不贴树皮/叶图）";
    }
    LOG(WARNING) << "读不到 shape（" << a.shape_path << "）→ 纯 glb 查看模式："
                 << "只有 glb 展示态，不能烘焙、不能回到原材料态";
  }
  if (!a.bake_glb_path.empty() && !std::ifstream(a.shape_path).good()) {
    LOG(FATAL) << "--bake 需要 shape（要从拓扑烘出 glb），读不到: " << a.shape_path;
  }
  return a;
}

}  // namespace
}  // namespace vegv2

using namespace vegv2;



int main(int argc, char** argv) {
  return vegv2::RunViewer(ParseCli(argc, argv));
}
