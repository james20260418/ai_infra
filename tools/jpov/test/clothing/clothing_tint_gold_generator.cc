// 顶点 tint gold 生成器：用 tinted 裙子资产渲三条路径，写仓库 gold。
//
// 用法（改场景后重出）：bazel run //tools/jpov/test/clothing:clothing_tint_gold_generator
#include <string>

#include <glog/logging.h>

#include "tools/jpov/test/clothing/clothing_tint_gold_common.h"

int main() {
    using jpov_cloth_tint_gold::Mode;
    for (Mode m : {Mode::kObject3d, Mode::kInstanced, Mode::kSkinned}) {
        const std::string gold =
            jpov::GetTestDataDir() + jpov_cloth_tint_gold::GoldRelPath(m);
        LOG(INFO) << "写 gold: " << gold;
        if (!jpov_cloth_tint_gold::RenderTintGold(m, /*tint_on=*/true, gold)) {
            LOG(FATAL) << "渲图失败: " << jpov_cloth_tint_gold::ModeName(m);
        }
    }
    return 0;
}
