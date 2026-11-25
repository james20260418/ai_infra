// JPOV Water Skin gold test（6 张固定机位，端到端逐像素基线）
//
// 保护对象：水皮 FS 拆成「水上/水下 × 生产/调试」四个 program + 共享 WaterSurface 结构体
// 的重构。重构必须**逐像素不变**；本测试用 1280×720 拍 6 张（水上 ×3 / 水下 ×3），
// 与仓库 gold PNG 逐像素比对（generator 产同一套机位）。
//
// 通过条件（双闸门，比 SSR 的闸门紧得多 —— 水皮是确定性单 pass，没有 llvmpipe 抖动）：
//   1) 粗粒度：8×8 ROI 均值最大通道差 <= 1（一个 LSB）；
//   2) 细粒度：逐像素显著变化（单通道差 > 1）占比 <= 0.1%。
// 水皮只要反射方向 / 菲涅尔 / 太阳项 / 深海积分 / α 任何一处走样，都会远超这两个阈值。

#include <cstdio>
#include <string>
#include <vector>

#include <glog/logging.h>
#include "third_party/stb/stb_image.h"

#include "tools/jpov/test/compare_light.h"
#include "tools/jpov/test/test_utils.h"
#include "tools/jpov/test/water/jpov_water_skin_gold_common.h"

namespace {

constexpr double kLightMeanThreshold = 1.0;    // 一个 LSB
constexpr int    kPixelDiffThreshold = 1;      // 单通道差 > 1 才计「变化」
constexpr double kMaxChangedRatio = 0.001;     // 0.1%

std::string GoldPath(const jpov_water_skin_gold::SkinGoldShot& shot) {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir != nullptr) {
        std::string p(test_srcdir);
        if (!p.empty() && p.back() != '/') {
            p.push_back('/');
        }
        return p + "__main__/tools/jpov/test/water/" +
               jpov_water_skin_gold::ShotFileName(shot);
    }
    return jpov::GetTestDataDir() + "/water/" + jpov_water_skin_gold::ShotFileName(shot);
}

}  // namespace

int main() {
    // gold 先检查存在（缺了直接报「去跑 generator」）。
    for (const jpov_water_skin_gold::SkinGoldShot& shot : jpov_water_skin_gold::kShots) {
        const std::string gold = GoldPath(shot);
        FILE* f = std::fopen(gold.c_str(), "rb");
        CHECK(f != nullptr) << "gold image 缺失，请先跑 jpov_water_skin_gold_generator: " << gold;
        std::fclose(f);
    }

    jpov_water_skin_gold::GoldHarness harness;
    harness.SetUp();

    int failures = 0;
    for (const jpov_water_skin_gold::SkinGoldShot& shot : jpov_water_skin_gold::kShots) {
        const std::string gold = GoldPath(shot);
        const std::vector<unsigned char> rendered = harness.RenderShot(shot);

        int w = 0;
        int h = 0;
        int comp = 0;
        unsigned char* gold_px = stbi_load(gold.c_str(), &w, &h, &comp, 4);
        CHECK(gold_px != nullptr) << "加载 gold PNG 失败: " << gold;
        CHECK_EQ(w, jpov_water_skin_gold::kWidth);
        CHECK_EQ(h, jpov_water_skin_gold::kHeight);
        const std::vector<unsigned char> gold_buf(
            gold_px, gold_px + static_cast<size_t>(w) * h * 4);
        stbi_image_free(gold_px);

        // 自检：必须有水皮覆盖（否则「全黑也全对」，空测）。
        int covered = 0;
        for (int i = 0; i < w * h; ++i) {
            if (rendered[static_cast<size_t>(i) * 4 + 3] > 0) {
                ++covered;
            }
        }
        if (covered == 0) {
            LOG(ERROR) << "[" << shot.name << "] 无任何水皮覆盖像素，用例失效";
            ++failures;
            continue;
        }

        const double mean_diff =
            jpov::CompareLightMeanRoi(gold_buf.data(), rendered.data(), w, h, 8, 8);
        const jpov::PixelChangeReport pr =
            jpov::ComparePixelChange(gold_buf.data(), rendered.data(), w, h,
                                     kPixelDiffThreshold);
        LOG(INFO) << "[" << shot.name << "] roi-mean-max=" << mean_diff
                  << " (<= " << kLightMeanThreshold << ")  changed="
                  << (pr.changed_ratio * 100.0) << "% (<= "
                  << (kMaxChangedRatio * 100.0) << "%)  max_diff=" << pr.max_diff
                  << "  covered=" << covered;

        if (mean_diff > kLightMeanThreshold || pr.changed_ratio > kMaxChangedRatio) {
            LOG(ERROR) << "[" << shot.name << "] 水皮回归（对比 gold " << gold << "）";
            ++failures;
        }
    }

    harness.TearDown();
    if (failures != 0) {
        LOG(ERROR) << failures << " / " << jpov_water_skin_gold::kShotCount
                   << " 个水皮 gold 组合不一致";
        return 1;
    }
    LOG(INFO) << "TEST PASSED: " << jpov_water_skin_gold::kShotCount
              << " 个水皮 gold 组合全部一致";
    return 0;
}
