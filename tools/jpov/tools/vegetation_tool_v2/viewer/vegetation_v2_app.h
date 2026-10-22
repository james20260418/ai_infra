// 植被 v2 查看器 —— **库接口**（实现全在 vegetation_v2_app.cc）。
//
// 定位：这是「植物组合查看器」的库入口。给我 shape / bark / leaf，给几样渲染几样：
//   `--shape` 省略用打包目录里的 shapes/willow.pbtxt；`--bark` / `--leaf` 省略就只渲染拓扑。
//
// 为什么接口只剩一个函数：查看器的场景/渲染/交互全在 .cc 里**只编译一次**——
// 曾经是 header-only，结果同一个实现被 viewer、gold test 各自编译一份，
// 叶卡 alpha 覆盖率 mip 的 CPU 二值化阈值因浮点细节不同而翻转 ⇒ 两边出图稳定差 12%
// （"gold 图和我在 viewer 里看到的不是一张图"）。现在所有 consumer 链接同一份代码。

#ifndef JPOV_TOOLS_VEGETATION_TOOL_V2_VIEWER_VEGETATION_V2_APP_H_
#define JPOV_TOOLS_VEGETATION_TOOL_V2_VIEWER_VEGETATION_V2_APP_H_

#include <string>

namespace vegv2 {

// 命令行参数（结构与语义见 vegetation_v2_viewer.cc 顶部的用法注释）。
struct CliArgs {
  std::string shape_path;
  std::string bark_dir;   // 空 = 不贴树皮（用 level 棕色）
  bool capture = false;
  std::string out_dir;
  double phi_deg = 12.0;
  double theta_deg = 35.0;
  double radius = 0.0;   // 0 = 自动
  int max_level = 99;
  float ground_y = 0.0f;
  float cam_target_y = 0.0f;   // 相机注视点高度（看树冠/叶卡细节用）
  bool show_panel = false;     // capture 时也画面板（截图/自检用）
  int only_type = -1;          // >=0：只显示该 leaf type（等价于面板取消"显示全部叶子"）
  bool hide_leaf = false;      // 强制看 debug 盒子（叶卡贴图关）
  bool hide_bark = false;      // 强制看 level 棕色（树皮关）
  bool hide_hud = false;       // 关掉左上角相机 HUD（gold 比对用，免得文字进基准图）
  // 法线贴图强度（uNormalScale）。默认 1.5：同一树皮 A/B 实测
  // 1.0 偏平、3.0 发塑料，1.5 最自然（Skylight + 单平行光下 ambient 占比高，
  // 1.0 的凹凸容易被环境光冲淡）。面板滑条同一量程。
  float normal_scale = 1.5f;
  // UV 的 v 方向缩放 = 贴图宽/高。0 = 自动从 albedo 的像素尺寸读。
  // 非方形贴图必须补偿，否则整张图被按正方形铺开：1:2 的图横向拉宽 2×、
  // 凸起在 u 方向被压扁，纹素不再方正（法线细节跟着糊）。
  float uv_vscale = 0.0f;

  // 叶卡（--leaf 给了才启用）。整张图 = 一格；黑底当透明。
  std::string leaf_path;
  std::string leaf2_path;   // leaf type 1（镜像手性）用的素材；空 = 回退 leaf_path
  float leaf_black = 12.0f;   // max(r,g,b) < 此值 → alpha 0（带 0.5x~1.5x 软过渡）
  float leaf_curl = 0.5f;     // A：顶点法线球面化强度
  float leaf_normal = 1.0f;   // B：法线贴图强度（uNormalScale）

  // ── GLB 烘焙（面板「烘焙 glb」用；也支持 headless 一条命令跑完）──
  std::string bake_glb_path;   // 非空 = 只烘焙这一次然后退出（CI / 命令行）
  std::string glb_path;        // 非空 = 启动即进「glb 展示态」（跳过原材料态）
  bool wind_off = false;       // 关掉 wind（A/B 用）
  float wind_time = -1.0f;     // >=0 = 钉住风时间（headless 确定性出图）
  float wind_amp = -1.0f;      // >=0 = 覆盖 pivot 振幅（A/B 用）
  float wind_turb = -1.0f;     // >=0 = 覆盖平动湍流幅度（A/B 用）
  float wind_blow = -1.0f;     // >=0 = 覆盖吹歪比例（A/B 用）
  float wind_blow_exp = -1.0f; // >=0 = 覆盖吹歪指数
};

// 跑查看器：`capture=false` 开交互窗口；`capture=true` 无窗口出三张图到 out_dir。
// 返回 0 = 正常；1 = shape 读不到/校验失败。
int RunViewer(const CliArgs& args);

}  // namespace vegv2

#endif  // JPOV_TOOLS_VEGETATION_TOOL_V2_VIEWER_VEGETATION_V2_APP_H_
