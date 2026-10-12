// 布料 LAG gold 测试 —— 给「**裙摆飘起**这件事**加单测保护**」（Danis 2026-10-12）。
//
// 两条门禁：
//   ① gold 比对（ROI 平铺均值，阈值 25；同其它 PBR/llvmpipe gold —— 逐像素在 llvmpipe 下不稳）：
//      固定设定下 LAG·一阶的渲图 = 仓库 gold。
//   ② **行为门禁**：开 LAG vs 关 LAG 的画面必须**明显不同** ⇒ 证明 LAG 真的让裙子「飘」起来
//      （若 LAG 没接上 / 松弛度全 0 / VS 没消费 pos_lag，这条立刻失败）。
#include <cstdlib>
#include <string>
#include <vector>

#include <glog/logging.h>
#include "third_party/stb/stb_image.h"

#include "tools/jpov/test/clothing/clothing_lag_gold_common.h"
#include "tools/jpov/test/compare_light.h"
#include "tools/jpov/test/test_utils.h"

namespace {
std::string GoldPath() {
    const char* e = std::getenv("TEST_SRCDIR");
    if (e) {
        std::string s = e;
        if (!s.empty() && s.back() != '/') {
            s.push_back('/');
        }
        return s + "__main__/tools/jpov/test" + jpov_cloth_lag_gold::GoldRelPath();
    }
    return jpov::GetTestDataDir() + jpov_cloth_lag_gold::GoldRelPath();
}
struct Img {
    int w = 0, h = 0;
    std::vector<unsigned char> px;
};
Img Load(const std::string& p) {
    Img im;
    int c = 0;
    unsigned char* d = stbi_load(p.c_str(), &im.w, &im.h, &c, 4);
    if (d == nullptr) {
        LOG(ERROR) << "读图失败: " << p;
        return im;
    }
    im.px.assign(d, d + static_cast<size_t>(im.w) * im.h * 4);
    stbi_image_free(d);
    return im;
}
}  // namespace

int main() {
    const std::string out_lag = jpov::GetOutputDir() + "cloth_lag_on.png";
    const std::string out_nolag = jpov::GetOutputDir() + "cloth_lag_off.png";
    CHECK(jpov_cloth_lag_gold::RenderClothLag(/*lag_on=*/true, /*second=*/false, out_lag))
        << "lag-on 渲图失败";
    CHECK(jpov_cloth_lag_gold::RenderClothLag(/*lag_on=*/false, /*second=*/false, out_nolag))
        << "lag-off 渲图失败";

    const Img gold = Load(GoldPath());
    const Img on = Load(out_lag);
    const Img off = Load(out_nolag);
    CHECK(gold.w > 0 && gold.h > 0) << "gold 缺失/读不出，请先跑 generator: " << GoldPath();
    CHECK(on.w == gold.w && on.h == gold.h) << "渲图与 gold 尺寸不一致";

    // ① gold 比对。
    jpov::MeanCompareReport rep;
    const double d = jpov::CompareLightMeanRoi(gold.px.data(), on.px.data(), gold.w, gold.h,
                                               /*nrow*/ 8, /*ncol*/ 8, &rep);
    LOG(INFO) << "gold ROI 最大通道差 = " << d;
    CHECK_LE(d, 25.0) << "LAG 渲图与 gold 差异过大（" << d << "），场景/链路可能变了";

    // ② 「裙摆飘起」行为门禁：开/关 LAG 的**变化像素数**（逐像素，能抓住裙摆的小位移）。
    //    （ROI 均值会把小位移平均掉，故这里数像素。）
    size_t changed = 0;
    for (size_t i = 0; i + 3 < on.px.size(); i += 4) {
        const int dr = std::abs(int(on.px[i]) - int(off.px[i]));
        const int dg = std::abs(int(on.px[i + 1]) - int(off.px[i + 1]));
        const int db = std::abs(int(on.px[i + 2]) - int(off.px[i + 2]));
        if (dr > 20 || dg > 20 || db > 20) {
            ++changed;
        }
    }
    LOG(INFO) << "lag-on vs lag-off 变化像素数 = " << changed;
    CHECK_GT(changed, 300u)
        << "开/关 LAG 画面几乎一样（变化像素 " << changed
        << "）—— 裙摆没飘起，LAG 可能没接上（松弛度 0 / VS 没消费 pos_lag）";
    LOG(INFO) << "TEST PASSED: 布料 LAG「裙摆飘起」";
    return 0;
}
