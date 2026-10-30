#include "tools/jpov/interface/debug_info.h"

#include <cstdio>
#include <string>
#include <vector>

namespace jpov {
namespace {

// 截断一条 GL 原始字符串到 kMaxGlFieldChars 个字符（**含**结尾的 "..."）。
// 空串原样返回（不变成 "..."）。
std::string LimitGlField(const std::string& s) {
    const std::size_t max_chars =
        static_cast<std::size_t>(DebugInfo::kMaxGlFieldChars);
    if (s.size() <= max_chars) {
        return s;
    }
    return s.substr(0, max_chars - 3) + "...";
}

// 行级硬护栏：无论上面怎么拼，**最终交给点阵字体画的每一行**都不超过
// kMaxLineChars（= 500px 预算）。这条在数字特别大时会真的触发
// （例如远平面 100000.00、或世界坐标上万），此时宁可切掉尾巴也不让 HUD 撑出屏幕。
std::string ClampLine(std::string line) {
    const std::size_t max_chars =
        static_cast<std::size_t>(DebugInfo::kMaxLineChars);
    if (line.size() <= max_chars) {
        return line;
    }
    return line.substr(0, max_chars - 3) + "...";
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
    // 为什么拆行而不是 "vendor X | GL Y" 合并：合并行在最坏情况下是两个 56 字符
    // 字段相加，必然超过 500px；拆行后每个字段独占一行，长度只受单字段上限约束。
    lines.emplace_back(std::string("GPU  ") + GpuKindName(gpu.kind));
    lines.emplace_back("     " + (gpu.renderer.empty() ? std::string("(unavailable)")
                                                        : LimitGlField(gpu.renderer)));
    lines.emplace_back("     vendor " +
                       (gpu.vendor.empty() ? std::string("?") : LimitGlField(gpu.vendor)));
    lines.emplace_back("     GL " + (gpu.version.empty()
                                         ? std::string("?")
                                         : LimitGlField(gpu.version)));
    lines.emplace_back(
        "     glsl " +
        (gpu.glsl_version.empty() ? std::string("?") : LimitGlField(gpu.glsl_version)));

    std::snprintf(buf, sizeof(buf), "     max_tex %d max_attribs %d max_color_attach %d",
                  gpu.max_texture_size, gpu.max_vertex_attribs, gpu.max_color_attachments);
    lines.emplace_back(buf);

    // ---- 相机机位（相机 HUD 的正式版本）----
    std::snprintf(buf, sizeof(buf), "CAM  pos (%.2f, %.2f, %.2f) -> (%.2f, %.2f, %.2f)",
                  camera.position.x(), camera.position.y(), camera.position.z(),
                  camera.target.x(), camera.target.y(), camera.target.z());
    lines.emplace_back(buf);

    std::snprintf(buf, sizeof(buf), "     up (%.2f, %.2f, %.2f) fov %.1f near %.2f far %.2f",
                  camera.up.x(), camera.up.y(), camera.up.z(), camera.fov,
                  camera.near, camera.far);
    lines.emplace_back(buf);

    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i != 0) {
            out += '\n';
        }
        out += ClampLine(lines[i]);
    }
    return out;
}

}  // namespace jpov
