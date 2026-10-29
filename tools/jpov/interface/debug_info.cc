#include "tools/jpov/interface/debug_info.h"

#include <cstdio>

namespace jpov {
namespace {

// 截断一条 GL 原始字符串到 kMaxGlFieldChars 个字符（超出部分用 "..." 结尾）。
// 空串原样返回（不变成 "..."）。
std::string LimitGlField(const std::string& s) {
    const std::size_t max_chars =
        static_cast<std::size_t>(DebugInfo::kMaxGlFieldChars);
    if (s.size() <= max_chars) {
        return s;
    }
    return s.substr(0, max_chars) + "...";
}

}  // namespace

std::string DebugInfo::DebugString() const {
    char buf[512];
    std::string out;

    // ---- 第一行：分辨率 / 帧号 / 帧率 ----
    std::snprintf(buf, sizeof(buf), "[JPOV] %.0fx%.0f frame %lld fps %.1f", width,
                  height, static_cast<long long>(frame_count), fps);
    out += buf;
    out += '\n';

    // ---- 硬件（GpuInfo）----
    out += "GPU  ";
    out += GpuKindName(gpu.kind);
    out += " | ";
    out += gpu.renderer.empty() ? std::string("(unavailable)") : LimitGlField(gpu.renderer);
    out += '\n';

    out += "     vendor ";
    out += gpu.vendor.empty() ? std::string("?") : LimitGlField(gpu.vendor);
    out += " | GL ";
    out += gpu.version.empty() ? std::string("?") : LimitGlField(gpu.version);
    out += '\n';

    out += "     glsl ";
    out += gpu.glsl_version.empty() ? std::string("?") : LimitGlField(gpu.glsl_version);
    out += '\n';

    std::snprintf(buf, sizeof(buf), "     max_tex %d max_attribs %d max_color_attach %d",
                  gpu.max_texture_size, gpu.max_vertex_attribs, gpu.max_color_attachments);
    out += buf;
    out += '\n';

    // ---- 相机机位（相机 HUD 的正式版本）----
    std::snprintf(buf, sizeof(buf), "CAM  pos (%.2f, %.2f, %.2f) -> (%.2f, %.2f, %.2f)",
                  camera.position.x(), camera.position.y(), camera.position.z(),
                  camera.target.x(), camera.target.y(), camera.target.z());
    out += buf;
    out += '\n';

    std::snprintf(buf, sizeof(buf),
                  "     up (%.2f, %.2f, %.2f) fov %.1f near %.2f far %.2f",
                  camera.up.x(), camera.up.y(), camera.up.z(), camera.fov,
                  camera.near, camera.far);
    out += buf;

    return out;
}

}  // namespace jpov
