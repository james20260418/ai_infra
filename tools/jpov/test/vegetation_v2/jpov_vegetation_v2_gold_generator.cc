// vegetation v2 查看器 gold image 生成器（写仓库里的两张基准图）。
//
// 用法（只在**故意**改观感/资产时跑，跑完把新图一起提交）：
//   bazel run //tools/jpov/test/vegetation_v2:jpov_vegetation_v2_gold_generator
//
// 产物（scene 参数在 vegetation_v2_gold_common.h，与 test 共用）：
//   vegetation_v2_cards_1280x720.png   debug 绿卡视图（精确锁叶卡摆放）
//   vegetation_v2_leaf_1280x720.png    贴叶图 + 树皮（烟雾报警：叶/树皮整片没了）
//   vegetation_v2_pine_game_1280x720.png  新生成的低配/游戏档资产（噪声+弯卡+三级环数）

#include <cstdio>
#include <string>

#include "tools/jpov/test/test_utils.h"
#include "tools/jpov/test/vegetation_v2/vegetation_v2_gold_common.h"

int main() {
  const std::string dir = jpov::GetTestDataDir() + "/vegetation_v2/";
  std::system(("mkdir -p " + dir).c_str());
  const std::string cards = dir + vegv2::goldtest::kCardsGold;
  const std::string leaf = dir + vegv2::goldtest::kLeafGold;
  if (!vegv2::goldtest::RenderGolden(/*with_leaf=*/false, cards)) {
    std::fprintf(stderr, "cards gold 生成失败\n");
    return 1;
  }
  if (!vegv2::goldtest::RenderGolden(/*with_leaf=*/true, leaf)) {
    std::fprintf(stderr, "leaf gold 生成失败\n");
    return 1;
  }
  const std::string glb = dir + vegv2::goldtest::kGlbGold;
  if (!vegv2::goldtest::RenderGlbGolden(glb)) {
    std::fprintf(stderr, "glb gold 生成失败\n");
    return 1;
  }
  const std::string game = dir + vegv2::goldtest::kGameGold;
  if (!vegv2::goldtest::RenderGameShapeGolden(game)) {
    std::fprintf(stderr, "pine_game gold 生成失败\n");
    return 1;
  }
  std::printf("gold images:\n  %s\n  %s\n  %s\n  %s\n", cards.c_str(), leaf.c_str(),
              glb.c_str(), game.c_str());
  return 0;
}
