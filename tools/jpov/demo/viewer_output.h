// JPOV 模型查看器 — 出图/合成共用产出工具（output 定位 + 目录 + ffmpeg）
//
// 架构治理动机（重构自 demo/jpov_model_viewer.cc 的历史形态，见
// docs/jpov_model_viewer_arch.md；旧版把 dir/base 推导 + mkdir + 递归删目录 +
// ffmpeg spawn 全部内联在 RunFourViews / RunRoundVideo 两个函数里，各写一遍、
// 且无法被单测覆盖。本次把其中可独立验证的纯逻辑/文件操作抽到本库共用，
// 配 viewer_output_test 单测，杜绝"下一种拍摄模式再抄第三次"。）
//
// 分工：
//   1. ResolveOutputTarget —— 纯函数：由 glTF 路径 + 可选 --output_dir，
//      推出「输出目录 dir + 模型 basename base」。不碰文件系统，可单测。
//   2. EnsureOutputDir —— 单层 mkdir（EEXIST 视为成功）；跨平台 _mkdir/mkdir。
//   3. MakeTempFramesDir / RemoveDirRecursive —— round_video 的临时 PNG 缓冲目录
//      生命周期（建 / 递归清）。跨平台：POSIX 真实现，Windows 空实现（占位）。
//   4. RunFfmpeg —— POSIX fork + execvp 调系统 ffmpeg 合成视频；不依赖 Bazel deps。
//
// 设计约定：本库只做"产出到磁盘/合成视频"这一件事，不含任何渲染/相机逻辑
// （渲染场景与拍摄模式driver见 viewer_app.h / viewer_capture.h，保持关注点分离；
// ffmpeg 是系统可执行文件，不纳入 Bazel 构建图）。

#ifndef JPOV_DEMO_VIEWER_OUTPUT_H_
#define JPOV_DEMO_VIEWER_OUTPUT_H_

#include <string>

namespace jpov_viewer {

// 从模型文件路径推出输出目标（目录 + basename，不含扩展名）。
//   explicit_dir  — --output_dir 显式指定（可为空 = 未指定）
//   返回 dir 决定产物写到哪、base 决定产物文件名前缀（<base>_<view>.png 等）。
struct OutputTarget {
    std::string dir;    // 产物输出目录（已解析，可能尚未创建，见 EnsureOutputDir）
    std::string base;   // 模型 basename（去目录、去最后一个后缀）
};

// 纯逻辑：由 glTF 路径 + 可选输出目录推出 dir/base。不碰文件系统。
//   - dir：explicit_dir 非空 → 用它；否则退回 glTF 所在目录（无目录 → "."）。
//   - base：恒取 glTF 文件名去掉最后一个 '.' 后缀之前的部分（与输出目录无关）。
// 该函数把原来 RunFourViews/RunRoundVideo 各自重复的 ~15 行推导统一到这里。
OutputTarget ResolveOutputTarget(const std::string& gltf_path,
                                 const std::string& explicit_dir);

// 单层 mkdir（父目录需已存在）。已存在（EEXIST）视为成功。
// 跨平台：POSIX mkdir(path,0755)；Windows _mkdir(path)。
// 失败（非 EEXIST）返回 false。
bool EnsureOutputDir(const std::string& path);

// round_video 专用：在输出目录下建临时帧缓冲子目录 "<base>_round_frames"。
// 父目录（输出目录）需已存在（调用方先 EnsureOutputDir 输出目录）。
// 已存在（EEXIST）视为成功。成功返回 true，失败返回 false。
bool EnsureTempFramesDir(const std::string& output_dir,
                         const std::string& base, std::string* out_frames_dir);

// 递归删除目录（round_video 合成完清掉临时 PNG 缓冲目录；无论 ffmpeg 成败都删）。
// 目录不存在时静默成功。POSIX opendir/readdir/unlink/rmdir；Windows 空实现
// （round_video 目标平台为 Linux/CI，Windows 路径当前不提供清理）。
void RemoveDirRecursive(const std::string& dir);

// 调系统 ffmpeg 把一批编号 PNG 帧合成 mp4（POSIX fork/execvp；Windows 占位失败）。
//   frame_pattern — ffmpeg -i 图像序列通配，如 "/path/frame_%04d.png"
//   out_mp4       — 视频输出路径
//   fps           — 视频帧率
// 成功返回 true；fork 失败或 ffmpeg 非零退出返回 false（并打 ERROR log）。
// 说明：ffmpeg 是系统可执行文件（apt 7:6.1.1），经 fork/execvp 按 PATH 找，
// 不进 Bazel deps。
bool RunFfmpeg(const std::string& frame_pattern, const std::string& out_mp4,
               int fps);

}  // namespace jpov_viewer

#endif  // JPOV_DEMO_VIEWER_OUTPUT_H_
