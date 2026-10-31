#include "tools/jpov/interface/debug_info.h"

#include <cstdio>
#include <string>
#include <vector>

namespace jpov {
namespace {

// 续行缩进：在原行前导空格之后再加 kContinuationIndent 个空格，形成挂缩进。
// 例（宽 61）：
//   "     GL 4.5 (Compatibility Profile) Mesa"
//   "         25.2.8-0ubuntu0.24.04.4"
constexpr int kContinuationIndent = 5;

// 把一行按宽度软换行 —— **不丢字符**（"..." 会把真信息藏掉）。
//
// 规则：
//   1. 优先在最后一个空格处断开（不把单词切成两半）；
//   2. 只有单词本身比一整行还长时才硬切；
//   3. 续行带挂缩进（原前导空格 + kContinuationIndent），所以续行可用宽度更小。
//
// Pre-condition: max_chars >= 2（每轮至少要能吃掉 1 个字符，否则不前进）
std::vector<std::string> WrapLine(const std::string& line, int max_chars) {
    const std::size_t width = static_cast<std::size_t>(max_chars);
    if (line.size() <= width) {
        return {line};
    }

    std::size_t indent_len = 0;
    while (indent_len < line.size() && line[indent_len] == ' ') {
        ++indent_len;
    }
    // 续行缩进最多占半行宽：缩进比可用宽度还大时 avail 会算出 0 甚至下溢，
    // 循环就永远前进不了（死循环）。我们的行前导空格 <= 5，这条护栏只在
    // 将来有人往 DebugString 里塞怪格式时才会真的生效。
    std::size_t cont_indent = indent_len + static_cast<std::size_t>(kContinuationIndent);
    if (cont_indent > width / 2) {
        cont_indent = width / 2;
    }
    const std::string continuation_prefix(cont_indent, ' ');

    std::vector<std::string> out;
    std::size_t pos = 0;
    std::string prefix;   // 首行不额外加缩进

    while (pos < line.size()) {
        const std::string& head = out.empty() ? prefix : continuation_prefix;
        const std::size_t avail = width - head.size();
        if (line.size() - pos <= avail) {
            out.push_back(head + line.substr(pos));
            break;
        }
        // 在 [pos, pos+avail] 内找最后一个空格（不能用它开头的空格当断点）。
        std::size_t cut = std::string::npos;
        for (std::size_t i = pos + avail; i > pos; --i) {
            if (line[i] == ' ') {
                cut = i;
                break;
            }
        }
        if (cut == std::string::npos) {
            cut = pos + avail;   // 单词比一行还长：硬切
        }
        std::string chunk = line.substr(pos, cut - pos);
        // 去掉断点处的尾部空格（下一行开头的空格也跳掉），避免白吃预算。
        while (!chunk.empty() && chunk.back() == ' ') {
            chunk.pop_back();
        }
        out.push_back(head + chunk);
        pos = cut;
        while (pos < line.size() && line[pos] == ' ') {
            ++pos;
        }
    }
    if (out.empty()) {
        out.push_back(line);
    }
    return out;
}

}  // namespace
std::string DebugInfo::DebugString() const {
    char buf[512];
    std::vector<std::string> lines;
    lines.reserve(10);

    // ---- 第一行：分辨率 / 帧号 / 帧率 ----
    std::snprintf(buf, sizeof(buf), "[JPOV] %.0fx%.0f frame %lld fps %.1f", width,
                  height, static_cast<long long>(frame_count), fps);
    lines.emplace_back(buf);

    // ---- 硬件（GpuInfo）：一个字段一行 ----
    // 一个字段一行（而不是 "vendor X | GL Y" 这种合并行）：字段之间不再互相挤占
    // 宽度预算，超长的那一个自己折行，短的那些不受影响。
    lines.emplace_back(std::string("GPU  ") + GpuKindName(gpu.kind));
    // 原始字符串**不截断**：超长就靠 WrapLine 折行，信息一个字符都不丢。
    lines.emplace_back("     " + (gpu.renderer.empty() ? std::string("(unavailable)")
                                                        : gpu.renderer));
    lines.emplace_back("     vendor " + (gpu.vendor.empty() ? std::string("?") : gpu.vendor));
    lines.emplace_back("     GL " + (gpu.version.empty() ? std::string("?") : gpu.version));
    lines.emplace_back("     glsl " +
                       (gpu.glsl_version.empty() ? std::string("?") : gpu.glsl_version));

    std::snprintf(buf, sizeof(buf), "     max_tex %d max_attribs %d max_color_attach %d",
                  gpu.max_texture_size, gpu.max_vertex_attribs, gpu.max_color_attachments);
    lines.emplace_back(buf);

    // ---- 相机机位（相机 HUD 的正式版本）----
    // ⭐ 数值用 **%.9g**（float 的 round-trip 精度：9 位有效数字足以让任何 float 原样
    //    打回），不再用 %.2f —— 调试时要能用这行把相机**原样复现**（Danis 2026-10-31：
    //    “把这个 cam 小数点精度增加到 float 精度”）。%.2f 会把 0.04214537 打成 0.04，
    //    掠射角下这点差别就是“能不能复现那个条纹”的差别。
    //    %.9g 不带多余的尾零（1.25 仍是 "1.25"），行超长时由 WrapLine 折行。
    std::snprintf(buf, sizeof(buf),
                  "CAM  pos (%.9g, %.9g, %.9g) -> (%.9g, %.9g, %.9g)",
                  camera.position.x(), camera.position.y(), camera.position.z(),
                  camera.target.x(), camera.target.y(), camera.target.z());
    lines.emplace_back(buf);

    std::snprintf(buf, sizeof(buf),
                  "     up (%.9g, %.9g, %.9g) fov %.9g near %.9g far %.9g",
                  camera.up.x(), camera.up.y(), camera.up.z(), camera.fov,
                  camera.near, camera.far);
    lines.emplace_back(buf);

    std::string out;
    bool first = true;
    for (const std::string& logical : lines) {
        for (const std::string& physical : WrapLine(logical, kMaxLineChars)) {
            if (!first) {
                out += '\n';
            }
            out += physical;
            first = false;
        }
    }
    return out;
}

}  // namespace jpov
