// leaf_card/jpov_leaf_card.cc — 叶卡归一化的命令行入口。
//
//   jpov_leaf_card <in.png> <out.png> [--anchor bottom_left] [--also-flip <out_flip.png>]
//
// 输入：生图模型出的「黑底叶子」（合同见 leaf_card_spec.h）
// 输出：合规素材（根点落在 anchor 角/边上、内容撑满宽度）+ 一份诊断（叶柄角度 /
//       旋转量 / 根点偏差 / 宽高比 / 灰区占比）。宽高比就是要写进 shape 的 leaf_aspect。

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "tools/jpov/tools/vegetation_tool_v2/leaf_card/leaf_card_normalize.h"
#include "tools/jpov/tools/vegetation_tool_v2/leaf_card/leaf_card_spec.h"

#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb/stb_image_write.h"

namespace {

const char* kUsage =
    "Usage: jpov_leaf_card <in.png> <out.png> [--anchor bottom_left|bottom_center|"
    "bottom_right|top_center] [--also-flip <out_flip.png>]\n"
    "\n"
    "把任意构图的黑底叶子图规整成合规叶卡素材：识别根部 → 旋转到 anchor 朝向 →\n"
    "AABB 裁剪，并打印诊断（叶柄角度 / 旋转量 / 根点偏差 / 宽高比）。\n"
    "生图合同（背景 <= 16、内容 >= 48，按 max(r,g,b)）见 leaf_card_spec.h。\n"
    "\n"
    "--also-flip 额外输出一张左右翻转的版本：资产格式要求「镜像手性」单独成一个 leaf\n"
    "type（引擎不做运行时镜像），而翻转会把 anchor_mode 从 bottom_left 换成\n"
    "bottom_right（反之亦然）。\n";

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fputs(kUsage, stderr);
    return 2;
  }
  const std::string in_path = argv[1];
  const std::string out_path = argv[2];
  std::string anchor = "bottom_left";
  std::string also_flip_path;
  for (int i = 3; i < argc; ++i) {
    if (std::strcmp(argv[i], "--anchor") == 0 && i + 1 < argc) {
      anchor = argv[++i];
    } else if (std::strcmp(argv[i], "--also-flip") == 0 && i + 1 < argc) {
      also_flip_path = argv[++i];
    } else {
      std::fprintf(stderr, "未知参数: %s\n%s", argv[i], kUsage);
      return 2;
    }
  }

  int width = 0;
  int height = 0;
  int channels = 0;
  unsigned char* raw = stbi_load(in_path.c_str(), &width, &height, &channels, 4);
  if (raw == nullptr) {
    std::fprintf(stderr, "读不了图片: %s\n", in_path.c_str());
    return 1;
  }
  jpov::vegetation::LeafCardImage input;
  input.width = width;
  input.height = height;
  input.rgba.assign(raw, raw + static_cast<std::size_t>(width) * height * 4);
  stbi_image_free(raw);

  jpov::vegetation::LeafCardNormalizeOptions options;
  options.anchor_mode = anchor;
  jpov::vegetation::LeafCardImage output;
  jpov::vegetation::LeafCardNormalizeReport report;
  std::string error;
  if (!jpov::vegetation::NormalizeLeafCard(input, options, &output, &report, &error)) {
    std::fprintf(stderr, "归一化失败: %s\n", error.c_str());
    return 1;
  }

  if (stbi_write_png(out_path.c_str(), output.width, output.height, 4, output.rgba.data(),
                     output.width * 4) == 0) {
    std::fprintf(stderr, "写不了输出: %s\n", out_path.c_str());
    return 1;
  }

  std::printf("[leaf] 输入 %dx%d（%.1f MB）→ 输出 %dx%d\n", input.width, input.height,
              input.rgba.size() / 1e6, output.width, output.height);
  std::printf("[leaf] 叶柄方向 %.1f°，旋转 %.1f° → anchor=%s\n", report.stem_angle_deg,
              report.rotation_deg, report.anchor_mode.c_str());
  std::printf("[leaf] 根点偏差 du=%+.3f dv=%+.3f（格内归一化，0 表示正好在锚点）\n",
              report.root_offset_u, report.root_offset_v);
  std::printf("[leaf] leaf_aspect = %.3f（宽/高，写进 shape）\n", report.aspect);

  if (!also_flip_path.empty()) {
    // 左右翻转 = 镜像手性。翻转后「根部所在的那个角」也左右换了，所以 anchor_mode 跟着换；
    // bottom_center / top_center 左右对称，保持不变。
    std::string mirrored_anchor = anchor;
    if (anchor == "bottom_left") {
      mirrored_anchor = "bottom_right";
    } else if (anchor == "bottom_right") {
      mirrored_anchor = "bottom_left";
    }
    jpov::vegetation::LeafCardImage flipped = output;
    for (int y = 0; y < output.height; ++y) {
      for (int x = 0; x < output.width; ++x) {
        const std::size_t source = (static_cast<std::size_t>(y) * output.width + x) * 4;
        const std::size_t target =
            (static_cast<std::size_t>(y) * output.width + (output.width - 1 - x)) * 4;
        for (int c = 0; c < 4; ++c) {
          flipped.rgba[target + static_cast<std::size_t>(c)] =
              output.rgba[source + static_cast<std::size_t>(c)];
        }
      }
    }
    if (stbi_write_png(also_flip_path.c_str(), flipped.width, flipped.height, 4,
                       flipped.rgba.data(), flipped.width * 4) == 0) {
      std::fprintf(stderr, "写不了翻转版本: %s\n", also_flip_path.c_str());
      return 1;
    }
    std::printf("[leaf] 镜像版本 → %s（leaf type 1，anchor_mode = %s）\n",
                also_flip_path.c_str(), mirrored_anchor.c_str());
  }
  if (report.ambiguous_fraction > 0.01) {
    std::printf("[leaf] ⚠ 灰区像素占内容 %.1f%%（背景/叶子亮度没守住生图合同，"
                "见 leaf_card_spec.h）\n", report.ambiguous_fraction * 100.0);
  }
  return 0;
}
