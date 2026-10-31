// JPOV DebugInfo 单元测试（纯 CPU，不渲染）
//
// 测什么：DebugString() 的**契约**——因为这份文本要被内置 8x8 点阵字体直接画到
// 屏幕上，凡是它不能表示的字符就会静默变成空白。所以这里最硬的一条断言是
// 「输出必须是纯可打印 ASCII（+ '\n'）」：任何将来往标签里塞中文/emoji 的改动
// 都会在这里挂掉，而不是在屏幕上悄悄变成空格。
//
// 断言清单：
//   1. 默认构造：kind=unknown、分辨率 0x0、帧 0、fps 0 都能被格式化出来，不崩
//   2. 字段确实进了文本（renderer/vendor/version/camera 的数值都能在输出里找到）
//   3. 纯 ASCII 且无 '\t'/'\r'，末尾不带换行
//   4. 超长 GL 字符串**换行而不截断**（一个字符都不许丢），且每行长度有界
//   5. 空 GL 字符串显示占位符（不出现空白的 "vendor  |" 这种断裂格式）

#include <cstdint>
#include <string>

#include <glog/logging.h>
#include <gtest/gtest.h>

#include "tools/jpov/interface/debug_info.h"

namespace {

// 输出里是否全为可打印 ASCII 或 '\n'。这是内置点阵字体的可渲染范围。
bool IsRenderableAscii(const std::string& s) {
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == '\n') {
            continue;
        }
        if (c < 32 || c > 126) {
            return false;
        }
    }
    return true;
}

TEST(DebugInfo, DefaultConstructedFormats) {
    const jpov::DebugInfo info;
    const std::string s = info.DebugString();

    EXPECT_NE(s.find("unknown"), std::string::npos)
        << "默认 kind 应为 kUnknown；实际输出：\n" << s;
    EXPECT_NE(s.find("0x0"), std::string::npos) << "未渲染时应显示 0x0；实际：\n" << s;
    EXPECT_NE(s.find("frame 0"), std::string::npos) << "实际：\n" << s;
    EXPECT_NE(s.find("fps 0.0"), std::string::npos) << "实际：\n" << s;
    EXPECT_TRUE(IsRenderableAscii(s)) << "输出含点阵字体画不出的字符：\n" << s;
}

TEST(DebugInfo, FieldsAppearInText) {
    jpov::DebugInfo info;
    info.gpu.kind = jpov::GpuKind::kCpuRaster;
    info.gpu.renderer = "llvmpipe (LLVM 15.0.7, 256 bits)";
    info.gpu.vendor = "Mesa/X.org";
    info.gpu.version = "4.5 (Core Profile) Mesa 23.0.4";
    info.gpu.glsl_version = "4.50";
    info.gpu.max_texture_size = 16384;
    info.gpu.max_vertex_attribs = 16;
    info.gpu.max_color_attachments = 8;
    info.camera.position = {1.25f, -2.5f, 3.0f};
    info.camera.target = {4.0f, 5.0f, 6.0f};
    info.camera.fov = 45.0f;
    info.camera.near = 0.5f;
    info.camera.far = 250.0f;
    info.width = 1280.0f;
    info.height = 720.0f;
    info.frame_count = 42;
    info.fps = 59.5f;

    const std::string s = info.DebugString();

    EXPECT_NE(s.find("cpu-raster"), std::string::npos) << s;
    EXPECT_NE(s.find("llvmpipe"), std::string::npos) << s;
    EXPECT_NE(s.find("Mesa/X.org"), std::string::npos) << s;
    EXPECT_NE(s.find("4.5 (Core Profile) Mesa 23.0.4"), std::string::npos) << s;
    EXPECT_NE(s.find("4.50"), std::string::npos) << s;
    EXPECT_NE(s.find("max_tex 16384"), std::string::npos) << s;
    EXPECT_NE(s.find("max_attribs 16"), std::string::npos) << s;
    EXPECT_NE(s.find("max_color_attach 8"), std::string::npos) << s;
    EXPECT_NE(s.find("1280x720"), std::string::npos) << s;
    EXPECT_NE(s.find("frame 42"), std::string::npos) << s;
    EXPECT_NE(s.find("fps 59.5"), std::string::npos) << s;
    EXPECT_NE(s.find("(1.25, -2.50, 3.00)"), std::string::npos) << s;
    EXPECT_NE(s.find("(4.00, 5.00, 6.00)"), std::string::npos) << s;
    EXPECT_NE(s.find("fov 45.0"), std::string::npos) << s;
    EXPECT_NE(s.find("near 0.50"), std::string::npos) << s;
    EXPECT_NE(s.find("far 250.00"), std::string::npos) << s;
    EXPECT_TRUE(IsRenderableAscii(s)) << s;
}

TEST(DebugInfo, NoTrailingNewlineAndNoControlChars) {
    jpov::DebugInfo info;
    info.gpu.renderer = "x";
    const std::string s = info.DebugString();

    ASSERT_FALSE(s.empty());
    EXPECT_NE(s.back(), '\n') << "末尾不该带换行（调用方按行切分更省事）";
    EXPECT_EQ(s.find('\r'), std::string::npos);
    EXPECT_EQ(s.find('\t'), std::string::npos);
    // 多行：至少 6 行（第一行 + 硬件 6 行 + 相机 2 行 = 9 行）
    int lines = 1;
    for (const char ch : s) {
        if (ch == '\n') {
            ++lines;
        }
    }
    EXPECT_GE(lines, 6) << "期望多行 HUD；实际：\n" << s;
}

TEST(DebugInfo, LongGlStringWrapsWithoutLosingChars) {
    jpov::DebugInfo info;
    const std::string blob(500, 'R');   // 远超一行
    info.gpu.renderer = blob;
    const std::string s = info.DebugString();

    // 1) 不截断：500 个 R 一个都不能少（"..." 会掩盖真信息，已废弃）。
    EXPECT_EQ(s.find("..."), std::string::npos) << "不该再出现截断标记；实际：\n" << s;
    std::size_t r_count = 0;
    for (const char ch : s) {
        if (ch == 'R') {
            ++r_count;
        }
    }
    EXPECT_EQ(r_count, blob.size()) << "换行过程丢了字符（" << r_count << "/"
                                     << blob.size() << "）：\n" << s;

    // 2) 每行仍受宽度契约约束。
    std::size_t line_len = 0;
    for (const char ch : s) {
        if (ch == '\n') {
            line_len = 0;
            continue;
        }
        ++line_len;
        EXPECT_LE(line_len, static_cast<std::size_t>(jpov::DebugInfo::kMaxLineChars))
            << "单行过长，HUD 会被撑出屏幕";
    }
}

// 换行必须发生在空格处（不把单词切两半），并给续行挂缩进。
TEST(DebugInfo, LongDriverStringWrapsAtSpacesWithHangingIndent) {
    jpov::DebugInfo info;
    // 57 字符、含空格：一整行放不下（61 - 5 缩进 - 3 "GL " = 53 可用）
    info.gpu.version = "4.5 (Compatibility Profile) Mesa 25.2.8-0ubuntu0.24.04.4";

    const std::string s = info.DebugString();
    std::vector<std::string> lines;
    std::string cur;
    for (const char ch : s) {
        if (ch == '\n') {
            lines.push_back(cur);
            cur.clear();
            continue;
        }
        cur += ch;
    }
    lines.push_back(cur);

    // 找以 "     GL " 开头的那条，它后面必须紧跟至少一条续行。
    std::size_t idx = lines.size();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].rfind("     GL ", 0) == 0) {
            idx = i;
            break;
        }
    }
    ASSERT_LT(idx, lines.size()) << "没找到 GL 行；实际：\n" << s;
    ASSERT_LT(idx + 1, lines.size()) << "这条 GL 行应该被折成两行；实际：\n" << s;

    // 续行 = 5(原缩进) + 5(挂缩进) 个空格开头。
    EXPECT_EQ(lines[idx + 1].rfind("          ", 0), 0u)
        << "续行缺挂缩进：[" << lines[idx + 1] << "]";
    // 断点落在空格处：两段拼回来必须等于原串（只多一个空格）。
    const std::string merged = lines[idx].substr(8) + " " + lines[idx + 1].substr(10);
    EXPECT_EQ(merged, info.gpu.version)
        << "换行没有在空格处断开（把单词切两半了）：\n" << merged << "\nvs\n"
        << info.gpu.version;
}

// 宽度契约（= 「debug 文字固定 500px」）：**每一条 GL 字段都撑到上限 + 世界坐标
// 上万 + 远平面 100000** 这种最坏组合下，单行仍然 <= kMaxLineChars。
// 这是唯一能保证「HUD 占地确定」的断言 —— 光靠"截断字段"管不住 snprintf 出来的
// 数字宽度（一度以为 5 空格缩进 + 56 字符就够了，实测 -12345.67 会顶破）。
TEST(DebugInfo, EveryLineFitsWidthContractEvenInWorstCase) {
    jpov::DebugInfo info;
    info.gpu.renderer = std::string(500, 'R');
    info.gpu.vendor = std::string(500, 'V');
    info.gpu.version = std::string(500, 'G');
    info.gpu.glsl_version = std::string(500, 'S');
    info.gpu.max_texture_size = 1073741824;
    info.gpu.max_vertex_attribs = 123456789;
    info.gpu.max_color_attachments = 987654321;
    info.camera.position = {-99999.99f, -88888.88f, -77777.77f};
    info.camera.target = {99999.99f, 88888.88f, 77777.77f};
    info.camera.up = {-1.0f, 0.5f, -0.5f};
    info.camera.fov = 179.9f;
    info.camera.near = 0.01f;
    info.camera.far = 100000.0f;
    info.width = 4294967295.0f;
    info.height = 4294967295.0f;
    info.frame_count = 9223372036854775807LL;
    info.fps = 9999.9f;

    const std::string s = info.DebugString();
    std::size_t line_len = 0;
    int line_no = 0;
    for (const char ch : s) {
        if (ch == '\n') {
            ++line_no;
            line_len = 0;
            continue;
        }
        ++line_len;
        EXPECT_LE(line_len, static_cast<std::size_t>(jpov::DebugInfo::kMaxLineChars))
            << "第 " << line_no << " 行超过 kMaxLineChars（= 500px 预算）：\n" << s;
    }
    EXPECT_TRUE(IsRenderableAscii(s)) << s;
}

TEST(DebugInfo, EmptyGlStringsUsePlaceholder) {
    jpov::DebugInfo info;   // 所有 GL 字符串为空
    const std::string s = info.DebugString();

    EXPECT_NE(s.find("(unavailable)"), std::string::npos)
        << "renderer 为空应显示占位符；实际：\n" << s;
    // 占位符为 "?"：一个 GL 字段一行，空串不产生断裂格式
    EXPECT_NE(s.find("vendor ?"), std::string::npos) << "实际：\n" << s;
    EXPECT_NE(s.find("GL ?"), std::string::npos) << "实际：\n" << s;
}

}  // namespace
