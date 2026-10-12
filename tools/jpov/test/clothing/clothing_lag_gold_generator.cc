// 布料 LAG gold 生成器：用裙子资产渲一帧（整体平动 LAG·一阶开）写仓库 gold。
//
// 用法（改场景后重出）：bazel run //tools/jpov/test/clothing:clothing_lag_gold_generator
#include <string>

#include <glog/logging.h>

#include "tools/jpov/test/clothing/clothing_lag_gold_common.h"

int main() {
    const std::string gold =
        jpov::GetTestDataDir() + jpov_cloth_lag_gold::GoldRelPath();
    LOG(INFO) << "写 gold: " << gold;
    if (!jpov_cloth_lag_gold::RenderClothLag(/*lag_on=*/true, /*second_order=*/false, gold)) {
        LOG(FATAL) << "渲图失败";
    }
    return 0;
}
