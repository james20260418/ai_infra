// image_input_test — 「提交前本地图片审查」的判定正确性门禁
//
// 守的是什么：
//   审查函数的唯一边界是「这张图一定不可能被受理 / 一定产出垃圾」。本测试要证明
//   ① 它**真的能拦住**该拦的（负向用例：不存在/空文件/格式不支持/超大/过小/视图数非法）；
//   ② 它**不会误拦**合法的（正向用例：正常 PNG/JPEG、恰好 2/4 张视图）；
//   ③ alpha 报告字段**如实**（决定底稿能否用透明底交付的判据依据）。
//
// 为什么必须有②：没有正向用例，「全部返回不通过」也能让负向用例通过——
//   那样测试是恒真的，等于没测。这是本项目踩过的坑（见
//   skills/zero-run-code-reading-check）。
//
// 测试数据全部在运行时用 stb_image_write 现场生成到临时目录，不依赖仓库二进制资源。

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <glog/logging.h>
#include <stb_image_write.h>

#include "tools/jpov/gen3d/static/image_input.h"

namespace {

using jpov::gen3d::CheckImageFile;
using jpov::gen3d::CheckImageFiles;
using jpov::gen3d::ImageCheckResult;
using jpov::gen3d::ImageLimits;

// 测试用临时目录（进程内唯一即可，用 /tmp 固定子目录避免污染仓库）。
constexpr char kTmpDir[] = "/tmp/jpov_gen3d_image_input_test";

// 造一个纯色 RGBA 图并写 PNG。alpha 值由参数控制（255=全不透明）。
// 返回写出的路径。
std::string WritePng(const std::string& name, int w, int h,
                     unsigned char r, unsigned char g, unsigned char b,
                     unsigned char a) {
    std::vector<unsigned char> px(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < px.size(); i += 4) {
        px[i + 0] = r;
        px[i + 1] = g;
        px[i + 2] = b;
        px[i + 3] = a;
    }
    const std::string path = std::string(kTmpDir) + "/" + name;
    const int rc = stbi_write_png(path.c_str(), w, h, 4, px.data(), w * 4);
    CHECK_EQ(rc, 1) << "写测试 PNG 失败: " << path;
    return path;
}

// 造一个纯色 RGB 图（无 alpha 通道）并写 JPEG。
std::string WriteJpegRgb(const std::string& name, int w, int h) {
    std::vector<unsigned char> px(static_cast<size_t>(w) * h * 3, 128);
    const std::string path = std::string(kTmpDir) + "/" + name;
    const int rc = stbi_write_jpg(path.c_str(), w, h, 3, px.data(), 90);
    CHECK_EQ(rc, 1) << "写测试 JPEG 失败: " << path;
    return path;
}

// 写一个指定字节数的垃圾文件（用于“非图片内容”用例）。
std::string WriteGarbage(const std::string& name, size_t n_bytes) {
    const std::string path = std::string(kTmpDir) + "/" + name;
    FILE* f = std::fopen(path.c_str(), "wb");
    CHECK(f != nullptr) << "无法创建垃圾文件: " << path;
    std::vector<char> junk(n_bytes, static_cast<char>(0x7A));  // 'z'
    if (n_bytes > 0) {
        std::fwrite(junk.data(), 1, n_bytes, f);
    }
    std::fclose(f);
    return path;
}

// 检查某结果是否以"包含某关键字"的错误被拒。
bool HasErrorContaining(const ImageCheckResult& r, const std::string& needle) {
    for (const std::string& e : r.errors) {
        if (e.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

int g_failed = 0;

void ExpectTrue(bool cond, const std::string& what) {
    if (cond) {
        std::fprintf(stderr, "  PASS: %s\n", what.c_str());
    } else {
        std::fprintf(stderr, "  FAIL: %s\n", what.c_str());
        ++g_failed;
    }
}

// ---- 正向用例：合法图必须通过 ----
void TestAcceptsValidPng() {
    std::fprintf(stderr, "[用例] 合法不透明 PNG 应通过\n");
    const std::string p = WritePng("valid_opaque.png", 512, 512, 200, 100, 50, 255);
    const ImageCheckResult r = CheckImageFile(p);
    ExpectTrue(r.ok, "不透明 512x512 PNG 判定为 ok");
    ExpectTrue(r.width == 512 && r.height == 512, "尺寸读出 512x512");
    ExpectTrue(r.channels == 4, "通道数读出 4");
    ExpectTrue(r.has_alpha_channel, "报告有 alpha 通道");
    ExpectTrue(!r.has_nontrivial_alpha, "全不透明图不应报「非平凡 alpha」");
    ExpectTrue(!r.has_transparent_pixels, "全不透明图不应报「含全透明像素」");
}

void TestAcceptsValidJpeg() {
    std::fprintf(stderr, "[用例] 合法无 alpha 的 JPEG 应通过\n");
    const std::string p = WriteJpegRgb("valid_rgb.jpg", 800, 600);
    const ImageCheckResult r = CheckImageFile(p);
    ExpectTrue(r.ok, "800x600 JPEG 判定为 ok");
    ExpectTrue(r.width == 800 && r.height == 600, "尺寸读出 800x600");
    ExpectTrue(!r.has_alpha_channel, "JPEG 无 alpha 通道");
}

// ---- alpha 报告如实（底稿方案判据）----
void TestReportsTransparentAlpha() {
    std::fprintf(stderr, "[用例] 含透明像素的 PNG 应如实报告 alpha 情况\n");
    const std::string p = WritePng("has_alpha0.png", 512, 512, 10, 20, 30, 0);
    const ImageCheckResult r = CheckImageFile(p);
    ExpectTrue(r.ok, "含 alpha=0 的 PNG 本身仍是合法图（ok）");
    ExpectTrue(r.has_alpha_channel, "报告有 alpha 通道");
    ExpectTrue(r.has_nontrivial_alpha, "报告含非 255 的 alpha 像素");
    ExpectTrue(r.has_transparent_pixels, "报告含全透明像素");
}

void TestReportsSemiTransparentAlpha() {
    std::fprintf(stderr, "[用例] 半透明（无全透明）应只报非平凡 alpha\n");
    const std::string p = WritePng("semi_alpha.png", 512, 512, 10, 20, 30, 128);
    const ImageCheckResult r = CheckImageFile(p);
    ExpectTrue(r.ok, "半透明 PNG 合法（ok）");
    ExpectTrue(r.has_nontrivial_alpha, "报告含非 255 的 alpha 像素");
    ExpectTrue(!r.has_transparent_pixels, "无 alpha=0 像素时不应报全透明");
}

// ---- 负向用例：该拦的一定要拦住 ----
void TestRejectsMissingFile() {
    std::fprintf(stderr, "[用例] 不存在的文件必须被拒\n");
    const ImageCheckResult r =
        CheckImageFile(std::string(kTmpDir) + "/definitely_not_here.png");
    ExpectTrue(!r.ok, "不存在的文件判定为不通过");
    ExpectTrue(HasErrorContaining(r, "不存在"), "错误信息指出文件不存在");
}

void TestRejectsEmptyFile() {
    std::fprintf(stderr, "[用例] 空文件必须被拒\n");
    const std::string p = WriteGarbage("empty.png", 0);
    const ImageCheckResult r = CheckImageFile(p);
    ExpectTrue(!r.ok, "空文件判定为不通过");
    ExpectTrue(HasErrorContaining(r, "为空"), "错误信息指出文件为空");
}

void TestRejectsGarbageContent() {
    std::fprintf(stderr, "[用例] 扩展名是 .png 但内容是垃圾，必须被拒\n");
    // 文件名像图片、内容不是图：必须先被拦下（不能只看扩展名）。
    const std::string p = WriteGarbage("fake.png", 4096);
    const ImageCheckResult r = CheckImageFile(p);
    ExpectTrue(!r.ok, "假 PNG（垃圾内容）判定为不通过");
    ExpectTrue(HasErrorContaining(r, "magic") || HasErrorContaining(r, "解码"),
               "错误信息指向格式/magic/解码问题");
}

// 这个用例是**唯一能把 magic bytes 检查单独逼出来**的：
//   内容是一个 stb 能正常解码、尺寸也合法的 BMP，但被故意命名为 .png。
//   此时：扩展名检查通过（.png）、解码通过、尺寸通过 —— 只有 magic bytes
//   检查能把它拦下。若哪天有人删掉 magic 检查，本用例会 FAIL（已负向验证）。
//   （注：前一个「假 PNG」用例被解码器兼住了，无法隔离 magic 检查。）
void TestRejectsMislabeledFormat() {
    std::fprintf(stderr, "[用例] 内容是 BMP 但命名为 .png，只能靠 magic bytes 拦下\n");
    // 用 stb 写一个真实 BMP，再拷成 .png 名字（内容不变）。
    const std::string bmp_path = std::string(kTmpDir) + "/real_content.bmp";
    {
        std::vector<unsigned char> px(static_cast<size_t>(512) * 512 * 3, 120);
        const int rc = stbi_write_bmp(bmp_path.c_str(), 512, 512, 3, px.data());
        CHECK_EQ(rc, 1) << "写测试 BMP 失败: " << bmp_path;
    }
    const std::string disguised =
        std::string(kTmpDir) + "/disguised_bmp_as.png";
    {
        FILE* in = std::fopen(bmp_path.c_str(), "rb");
        CHECK(in != nullptr) << "无法读 " << bmp_path;
        FILE* out = std::fopen(disguised.c_str(), "wb");
        CHECK(out != nullptr) << "无法写 " << disguised;
        char buf[4096];
        size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0) {
            std::fwrite(buf, 1, n, out);
        }
        std::fclose(in);
        std::fclose(out);
    }
    const ImageCheckResult r = CheckImageFile(disguised);
    ExpectTrue(!r.ok, "BMP 内容 + .png 扩展名判定为不通过");
    // 关键：拒因必须来自 magic bytes，而不是扩展名（扩展名是合法的 .png）。
    ExpectTrue(HasErrorContaining(r, "magic"),
               "拒因来自 magic bytes 检查（而非扩展名）");
    ExpectTrue(!HasErrorContaining(r, "扩展名"),
               "不应报扩展名问题（.png 本身合法）—— 证明是 magic 拦下的");
}

void TestRejectsUnsupportedExtension() {
    std::fprintf(stderr, "[用例] 不支持的扩展名必须被拒\n");
    const std::string p = WritePng("tricky.bmp", 512, 512, 1, 2, 3, 255);
    const ImageCheckResult r = CheckImageFile(p);
    // 内容是合法 PNG，但扩展名 .bmp 不受支持 → 扩展名检查应拦下。
    ExpectTrue(!r.ok, ".bmp 扩展名判定为不通过");
    ExpectTrue(HasErrorContaining(r, "扩展名"), "错误信息指出扩展名问题");
}

void TestRejectsTooSmall() {
    std::fprintf(stderr, "[用例] 分辨率过小必须被拒\n");
    const std::string p = WritePng("tiny.png", 64, 64, 1, 2, 3, 255);
    const ImageCheckResult r = CheckImageFile(p);
    ExpectTrue(!r.ok, "64x64 判定为不通过");
    ExpectTrue(HasErrorContaining(r, "过小"), "错误信息指出分辨率过小");
}

void TestRejectsTooLargeFile() {
    std::fprintf(stderr, "[用例] 文件超过字节上限必须被拒\n");
    // 用极小的 max_file_bytes 触发，避免真造 20MB 文件。
    ImageLimits tight;
    tight.max_file_bytes = 100;  // 100 字节上限
    const std::string p = WritePng("big_for_limit.png", 512, 512, 1, 2, 3, 255);
    const ImageCheckResult r = CheckImageFile(p, tight);
    ExpectTrue(!r.ok, "超过字节上限判定为不通过");
    ExpectTrue(HasErrorContaining(r, "过大"), "错误信息指出文件过大");
}

void TestRejectsTooHighRes() {
    std::fprintf(stderr, "[用例] 分辨率超上限必须被拒\n");
    ImageLimits tight;
    tight.max_edge_px = 100;  // 上限 100px
    const std::string p = WritePng("too_high_res.png", 512, 512, 1, 2, 3, 255);
    const ImageCheckResult r = CheckImageFile(p, tight);
    ExpectTrue(!r.ok, "512px 超过 100px 上限判定为不通过");
    ExpectTrue(HasErrorContaining(r, "过大"), "错误信息指出分辨率过大");
}

// ---- 多视图组合检查 ----
void TestMultiviewAcceptsTwoToFour() {
    std::fprintf(stderr, "[用例] 多视图 2~4 张合法图应通过\n");
    const std::string a = WritePng("mv_front.png", 512, 512, 10, 10, 10, 255);
    const std::string b = WritePng("mv_left.png", 512, 512, 20, 20, 20, 255);
    const std::string c = WritePng("mv_back.png", 512, 512, 30, 30, 30, 255);
    const std::string d = WritePng("mv_right.png", 512, 512, 40, 40, 40, 255);

    const std::vector<ImageCheckResult> two = CheckImageFiles({a, b});
    ExpectTrue(two.size() == 2 && two[0].ok && two[1].ok, "2 张视图通过");

    const std::vector<ImageCheckResult> four = CheckImageFiles({a, b, c, d});
    bool all_ok = (four.size() == 4);
    for (const ImageCheckResult& r : four) {
        all_ok = all_ok && r.ok;
    }
    ExpectTrue(all_ok, "4 张视图通过");
}

void TestMultiviewRejectsTooFew() {
    std::fprintf(stderr, "[用例] 多视图仅 1 张必须被拒\n");
    const std::string a = WritePng("mv_only_one.png", 512, 512, 10, 10, 10, 255);
    const std::vector<ImageCheckResult> one = CheckImageFiles({a});
    ExpectTrue(!one.empty() && !one[0].ok, "单张视图判定为不通过");
    ExpectTrue(HasErrorContaining(one[0], "视图数量"), "错误信息指出视图数量非法");
}

void TestMultiviewRejectsTooMany() {
    std::fprintf(stderr, "[用例] 多视图超过 4 张必须被拒\n");
    std::vector<std::string> five;
    for (int i = 0; i < 5; ++i) {
        five.push_back(WritePng("mv_many_" + std::to_string(i) + ".png",
                                512, 512, 10, 10, 10, 255));
    }
    const std::vector<ImageCheckResult> r = CheckImageFiles(five);
    ExpectTrue(!r.empty() && !r[0].ok, "5 张视图判定为不通过");
    ExpectTrue(HasErrorContaining(r[0], "视图数量"), "错误信息指出视图数量非法");
}

// 单图链路专用的抗回归用例：单张合法图必须能通过 CheckImageFile，
// 不得被多视图的“至少 2 张”规则误拦。
// （真实缺陷：gen3d_cmd 最初在单图模式也调了 CheckImageFiles，导致合法单图
//   被“视图数量非法：1 张”拒掉——本用例就是那次修复的回归护栏。）
void TestSingleImageNotBlockedByViewCountRule() {
    std::fprintf(stderr, "[用例] 单图合法图不应被“至少 2 张”规则误拦\n");
    const std::string p = WritePng("single_ok.png", 512, 512, 90, 90, 90, 255);
    // 单图链路应使用 CheckImageFile（单张），而非 CheckImageFiles（多视图组合）。
    const ImageCheckResult one = CheckImageFile(p);
    ExpectTrue(one.ok, "单张合法图经 CheckImageFile 判定为 ok");
    // 作为对比，同一张图走多视图组合接口确实会被数量规则拦下——
    // 这证明两个接口语义确实不同（而不是两者恰好行为一致）。
    const std::vector<ImageCheckResult> as_mv = CheckImageFiles({p});
    ExpectTrue(!as_mv.empty() && !as_mv[0].ok,
               "同一张图走 CheckImageFiles（多视图）会被数量规则拦下");
    ExpectTrue(HasErrorContaining(as_mv[0], "视图数量"),
               "多视图组合接口的拒因为视图数量");
}

void TestMultiviewRejectsBadFront() {
    std::fprintf(stderr, "[用例] front 不合格则整体必须被拒\n");
    const std::string bad_front =
        WritePng("mv_bad_front.png", 64, 64, 10, 10, 10, 255);  // 过小
    const std::string good = WritePng("mv_good_left.png", 512, 512, 20, 20, 20, 255);
    const std::vector<ImageCheckResult> r = CheckImageFiles({bad_front, good});
    ExpectTrue(r.size() == 2, "返回结果数与输入一致");
    ExpectTrue(!r[0].ok, "front（过小）判定为不通过");
    ExpectTrue(r[1].ok, "left（合法）本身仍判定为 ok");
}

}  // namespace

int main() {
    // 准备临时目录。
    const std::string mk = std::string("mkdir -p ") + kTmpDir;
    CHECK_EQ(std::system(mk.c_str()), 0) << "无法创建临时目录 " << kTmpDir;

    TestAcceptsValidPng();
    TestAcceptsValidJpeg();
    TestReportsTransparentAlpha();
    TestReportsSemiTransparentAlpha();
    TestRejectsMissingFile();
    TestRejectsEmptyFile();
    TestRejectsGarbageContent();
    TestRejectsMislabeledFormat();
    TestRejectsUnsupportedExtension();
    TestRejectsTooSmall();
    TestRejectsTooLargeFile();
    TestRejectsTooHighRes();
    TestMultiviewAcceptsTwoToFour();
    TestMultiviewRejectsTooFew();
    TestMultiviewRejectsTooMany();
    TestMultiviewRejectsBadFront();
    TestSingleImageNotBlockedByViewCountRule();

    if (g_failed != 0) {
        std::fprintf(stderr, "\n%d 个断言失败\n", g_failed);
        return 1;
    }
    std::fprintf(stderr, "\n全部通过\n");
    return 0;
}
