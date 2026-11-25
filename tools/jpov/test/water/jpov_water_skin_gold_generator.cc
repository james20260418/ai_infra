// JPOV Water Skin gold image generator（6 张固定机位：水上 ×3 + 水下 ×3）
//
// 产物：tools/jpov/test/water/water_skin_gold_<case>_1280x720.png
// 供 jpov_water_skin_gold_test 做「水皮 FS 拆分前后逐像素不变」的端到端保护。
// （机位 / 输入定义见 jpov_water_skin_gold_common.h）
//
// 用法：bazel run //tools/jpov/test/water:jpov_water_skin_gold_generator
//
// ⚠️ 不在这里定义 STB_IMAGE_WRITE_IMPLEMENTATION：本 target 链接了 //tools/jpov/include/jpov
//   （其依赖链里有 orm_unpack.cc，已带一份实现），再定义会重复符号。stbi_write_png 符号
//   由 orm_unpack 提供，这里只 include 头。

#include <string>

#include <glog/logging.h>
#include "third_party/stb/stb_image_write.h"

#include "tools/jpov/test/test_utils.h"
#include "tools/jpov/test/water/jpov_water_skin_gold_common.h"

int main() {
    jpov_water_skin_gold::GoldHarness harness;
    harness.SetUp();

    const std::string dir = jpov::GetTestDataDir() + "/water/";
    for (const jpov_water_skin_gold::SkinGoldShot& shot : jpov_water_skin_gold::kShots) {
        const std::vector<unsigned char> img = harness.RenderShot(shot);
        const std::string path = dir + jpov_water_skin_gold::ShotFileName(shot);
        const int rc = stbi_write_png(path.c_str(), jpov_water_skin_gold::kWidth,
                                      jpov_water_skin_gold::kHeight, 4, img.data(),
                                      jpov_water_skin_gold::kWidth * 4);
        CHECK_EQ(rc, 1) << "写 gold PNG 失败: " << path;
        LOG(INFO) << "gold generated: " << path;
    }

    harness.TearDown();
    LOG(INFO) << "water skin gold generator done (" << jpov_water_skin_gold::kShotCount
              << " shots)";
    return 0;
}
