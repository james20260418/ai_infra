// vegetation v2 查看器 gold 场景 —— test 与 generator 共用的同一份构建代码。
//
// **关键决定：gold 由「查看器二进制本身」跑出来**（subprocess + `--capture`），
// 不是 test 里自己 new 一个 app。
//
// 为什么：曾经在 test 里直接驱动 app，结果 gold 图和用户在 viewer 里看到的
// 有一处稳定差异（叶卡区域 12% 像素不同、叶子明显偏暗），而**几何完全一致**、
// 参数逐项相同（字体、HUD、warmup 帧数、相机全都试过）。根因是「同一份 header
// 代码被两个不同编译选项的 target 各编译一次」（叶卡的 alpha 覆盖率 mip 是 CPU 侧
// 二值化阈值，浮点细微差别会翻转 texel）——只要两边不是同一个二进制，就有这种
// 幽灵差异。gold 是给人肉眼验收用的，就必须等于用户看到的那张图。
//
// 两张 gold，各自保护不同的东西：
//
//   cards_1280x720.png  ← debug 绿卡视图（不贴叶图）：
//       卡是**不透明纯色盒子**，llvmpipe 下逐字节确定（实测连续 6 次 diff = 0）。
//       精确锁住「叶卡摆在哪儿」：anchor / up / normal / width / height 与
//       anchor_mode → UV 的推导。改错半张卡在这张图上就是明显差异。
//
//   leaf_1280x720.png   ← 贴叶图（--leaf-black 30）+ 树皮：
//       叶卡是 alpha cutout + 覆盖率 mip，llvmpipe 下会落在几种离散状态
//       （实测同一二进制多次运行，8×8 ROI 均值差 0 ~ 22 跳），所以只能宽阈值
//       当烟雾报警：叶子整片消失 64、树皮掉了 46 会被抓住。
//
// 机位是**近景**（R=1.6，cam_target_y=3）：远景时叶子只占画面 ~2%，连「叶卡横移
// 半张卡」都只有 3.7 的均值差，压不住噪声；近景能放大到 16.9 / 38.7。

#ifndef JPOV_TOOLS_VEGETATION_TOOL_V2_GOLD_COMMON_H_
#define JPOV_TOOLS_VEGETATION_TOOL_V2_GOLD_COMMON_H_

#include <cstdio>
#include <cstdlib>
#include <string>

#include <glog/logging.h>

#include "tools/jpov/test/test_utils.h"

namespace vegv2 {
namespace goldtest {

// bazel runfiles 根（`$TEST_SRCDIR/__main__`），本地 `bazel run` 时退化成仓库根。
inline std::string RunfilesRoot() {
  const char* srcdir = std::getenv("TEST_SRCDIR");
  if (srcdir != nullptr) {
    std::string p = srcdir;
    if (!p.empty() && p.back() != '/') {
      p.push_back('/');
    }
    return p + "__main__/";
  }
  return jpov::GetProjectRoot();
}

// 样例资产目录（shape + leaf + bark 都在这里）。
inline std::string SampleAssetDir() {
  return RunfilesRoot() + "tools/jpov/tools/vegetation_tool_v2";
}

// 查看器二进制（gold 就用它跑，保证与用户看到的一致）。
//
// 三种位置都试一遍：runfiles（bazel test）→ bazel-bin（bazel run）→ 仓库内路径。
inline std::string ViewerBinary() {
  const std::string rel = "tools/jpov/tools/vegetation_tool_v2/jpov_vegetation_v2_viewer";
  const std::string cands[] = {
      RunfilesRoot() + rel,
      jpov::GetProjectRoot() + "bazel-bin/" + rel,
      jpov::GetProjectRoot() + rel,
  };
  for (const std::string& c : cands) {
    if (FILE* f = std::fopen(c.c_str(), "rb")) {
      std::fclose(f);
      return c;
    }
  }
  LOG(ERROR) << "找不到查看器二进制，试过:" << "\n  " << cands[0] << "\n  " << cands[1]
             << "\n  " << cands[2];
  return cands[0];
}

// 出图分辨率（与查看器默认一致）。
constexpr int kWidth = 1280;
constexpr int kHeight = 720;

// 出图文件（相对 gold 目录 / output 目录）。
constexpr const char* kCardsGold = "vegetation_v2_cards_1280x720.png";
constexpr const char* kLeafGold = "vegetation_v2_leaf_1280x720.png";
// glb 展示态（烘焙 → 重新载入 → 带风渲染）。锁住整条烘焙链路：
// 关节表 / 逐顶点 (joint,phase) / 内嵌贴图 / 两关节混合 / 吹歪。
constexpr const char* kGlbGold = "vegetation_v2_glb_1280x720.png";

// 风时间（秒）：**必须钉住**，否则每帧累加导致两次渲染必不相同。
constexpr float kGlbWindTime = 0.8f;

// 跑一遍 gold 场景，把 front 视角的图拷到 out_png。返回 false = 失败。
//
// with_leaf=false → debug 绿卡视图（--no-leaf）；true → 贴叶图 + 树皮。
// 说明：viewer 的 --capture 会按 front/three_quarter/side 各拍一张，这里只取 front。
inline bool RenderGolden(bool with_leaf, const std::string& out_png) {
  const std::string dir = SampleAssetDir();
  const std::string shot_dir = out_png + ".shots";
  std::system(("rm -rf '" + shot_dir + "'").c_str());

  // 相机与材质参数和 `build_jpov_vegetation_v2_viewer.sh` 打印的完整组合命令一致。
  //
  // ⚠️ **所有会影响像素的渲染参数都必须在这里显式钉住**，不能吃 app 的默认值：
  //    2026-10-22 只是把树皮法线强度默认值从 1.5 改成 0.75（Danis 要求），
  //    两张 gold 就整体变了、必须重新生成 —— 而 gold 是「有没有回归」的唯一依据，
  //    它跟着默认值漂移就等于失去报警能力。
  //    `--uv-vscale 0.5` = 树皮 albedo 512x1024 的自动值（宽/高），钉住以免改自动公式时漂移。
  std::string cmd = "'" + ViewerBinary() + "'";
  cmd += " --shape '" + dir + "/samples/willow.pbtxt'";
  cmd += " --bark '" + dir + "/assets/bark/bark001'";
  cmd += " --normal-scale 0.75 --uv-vscale 0.5";
  if (with_leaf) {
    // 这批样例叶卡背景是 ≈10~13 的灰噪、内容 ≥44 ⇒ 软过档 [0.5t, 1.5t] 取 t=30 得 [15,45]。
    cmd += " --leaf '" + dir + "/assets/leaf/willow_leaf.png' --leaf-black 30 --leaf-normal 1.0";
  } else {
    cmd += " --no-leaf";
  }
  cmd += " --cam_phi 12 --cam_theta 0 --cam_r 1.6 --cam-target-y 3";
  // 原材料态从 2026-10-26 起也会吹风（关节表在本地现建）。这两张 gold 锁的是
  // **叶卡几何 / 贴图 / 树皮**，不是风，而且风位移随帧累加 ⇒ 必须关掉。
  // 不能靠 `--wind-time 0` 把位移钉成零：θ = amp·sin(2π·f·(t + 关节相位))，
  // 逐关节相位非零 ⇒ t=0 仍然在吹（实测 cards ROI 差 16.3）。
  cmd += " --no-wind";
  cmd += " --no-hud";   // HUD 是文字，会跟着相机字符串变，不该进基准图
  cmd += " --capture '" + shot_dir + "'";
  cmd += " > /dev/null 2>&1";

  const int rc = std::system(cmd.c_str());
  if (rc != 0) {
    LOG(ERROR) << "查看器返回 " << rc << "：" << cmd;
    return false;
  }
  const std::string front = shot_dir + "/veg_v2_front.png";
  const int cp = std::system(("cp '" + front + "' '" + out_png + "'").c_str());
  if (cp != 0) {
    LOG(ERROR) << "没拿到 front 图: " << front;
    return false;
  }
  std::system(("rm -rf '" + shot_dir + "'").c_str());
  LOG(INFO) << "gold 场景已渲染（" << (with_leaf ? "贴叶图" : "debug 绿卡") << "）: " << out_png;
  return true;
}

// 跑一遍 **glb 展示态** gold：先烘焙一份 glb，再用查看器 `--glb` 载入并出图。
//
// 为什么不 in-process 烘焙：gold 必须等于用户看到的那张图。烘焙也用**同一个查看器
// 二进制**的 `--bake`（与用户在面板上点「烘焙 glb」走同一份代码），所以整条链——
// 关节构造 → 写 glb → 重新载入 → 两关节混合蒙皮 → 风/吹歪——都在基准图里。
//
// 确定性：风时间用 `--wind-time` 钉住（否则每帧累加），实测同一二进制多次渲染 md5 相同。
inline bool RenderGlbGolden(const std::string& out_png) {
  const std::string dir = SampleAssetDir();
  const std::string shot_dir = out_png + ".shots";
  const std::string glb = out_png + ".glb";
  std::system(("rm -rf '" + shot_dir + "'").c_str());

  // 1) 烘焙（与面板默认参数一致：normal-scale 0.75、uv 自动=0.5、叶黑阈 30）。
  std::string bake = "'" + ViewerBinary() + "'";
  bake += " --shape '" + dir + "/samples/willow.pbtxt'";
  bake += " --bark '" + dir + "/assets/bark/bark001'";
  bake += " --leaf '" + dir + "/assets/leaf/willow_leaf.png' --leaf-black 30";
  bake += " --normal-scale 0.75 --uv-vscale 0.5";
  bake += " --bake '" + glb + "'";
  bake += " > /dev/null 2>&1";
  const int bake_rc = std::system(bake.c_str());
  if (bake_rc != 0) {
    LOG(ERROR) << "烘焙 glb 失败（返回 " << bake_rc << "）：" << bake;
    return false;
  }
  {
    FILE* f = std::fopen(glb.c_str(), "rb");
    if (f == nullptr) {
      LOG(ERROR) << "烘焙没有产出 glb: " << glb;
      return false;
    }
    std::fclose(f);
  }

  // 2) glb 展示态出图（相机用自动取景：与用户 `--glb` 打开时看到的一致）。
  const std::string wind = std::to_string(kGlbWindTime);
  std::string cmd = "'" + ViewerBinary() + "'";
  // **故意不给 --shape**：gold 走「纯 glb 查看模式」（runfiles 的 cwd 里没有
  // shapes/willow.pbtxt，缺 shape 即进入该模式）。这样这张 gold 同时守住了
  // 「只拿一个 glb 也能打开」这条路径 —— 它与"shape + glb"渲染逐像素一致（实测 0 差异）。
  cmd += " --glb '" + glb + "'";
  cmd += " --wind-time " + wind;
  // 相机**固定**（自动取景 R=17.88 太远，叶子只剩几个像素，回归看不出来）。
  // 这组参数把整棵树放进画面、同时让叶卡有十几像素宽 —— 风/吹歪看枝条摆幅，
  // 叶子看 cutout 与贴图。改这里 = 换构图，必须重出 gold。
  cmd += " --cam_phi 15 --cam_theta 25 --cam_r 8 --cam-target-y 4.5";
  cmd += " --no-hud";   // HUD 是文字，且含相机字符串，不进基准图
  cmd += " --capture '" + shot_dir + "'";
  cmd += " > /dev/null 2>&1";
  const int rc = std::system(cmd.c_str());
  if (rc != 0) {
    LOG(ERROR) << "查看器返回 " << rc << "：" << cmd;
    return false;
  }
  const std::string front = shot_dir + "/veg_v2_front.png";
  const int cp = std::system(("cp '" + front + "' '" + out_png + "'").c_str());
  if (cp != 0) {
    LOG(ERROR) << "没拿到 front 图: " << front;
    return false;
  }
  std::system(("rm -rf '" + shot_dir + "'").c_str());
  std::system(("rm -f '" + glb + "'").c_str());
  LOG(INFO) << "glb gold 场景已渲染（烘焙 + 载入 + 风）: " << out_png;
  return true;
}

}  // namespace goldtest
}  // namespace vegv2
#endif  // JPOV_TOOLS_VEGETATION_TOOL_V2_GOLD_COMMON_H_
