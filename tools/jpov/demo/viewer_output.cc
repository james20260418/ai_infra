// viewer_output.cc — 见 viewer_output.h。
//
// 纯文件系统/进程工具，无渲染依赖，可被 viewer_output_test 单测。

#include "tools/jpov/demo/viewer_output.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#ifndef _WIN32
#include <dirent.h>   // RemoveDirRecursive (opendir/readdir/unlink/rmdir)
#include <unistd.h>   // fork/execvp/unlink/rmdir
#include <sys/stat.h> // mkdir
#include <sys/wait.h> // waitpid
#endif
#ifdef _WIN32
#include <direct.h>  // _mkdir
#endif

#include <glog/logging.h>

namespace jpov_viewer {

OutputTarget ResolveOutputTarget(const std::string& gltf_path,
                                 const std::string& explicit_dir) {
    OutputTarget out;
    const size_t slash = gltf_path.find_last_of("/\\");
    out.dir = !explicit_dir.empty()
                  ? explicit_dir
                  : (slash == std::string::npos)
                        ? "." : gltf_path.substr(0, slash);
    const std::string full =
        (slash == std::string::npos) ? gltf_path : gltf_path.substr(slash + 1);
    const size_t dot = full.find_last_of('.');
    out.base = (dot == std::string::npos) ? full : full.substr(0, dot);
    return out;
}

bool EnsureOutputDir(const std::string& path) {
#ifdef _WIN32
    const int rc = _mkdir(path.c_str());
#else
    const int rc = mkdir(path.c_str(), 0755);
#endif
    return (rc == 0 || errno == EEXIST);
}

bool EnsureTempFramesDir(const std::string& output_dir,
                         const std::string& base, std::string* out_frames_dir) {
    const std::string frames_dir = output_dir + "/" + base + "_round_frames";
#ifdef _WIN32
    const int rc = _mkdir(frames_dir.c_str());
#else
    const int rc = mkdir(frames_dir.c_str(), 0755);
#endif
    if (rc != 0 && errno != EEXIST) {
        LOG(ERROR) << "无法创建临时帧目录: " << frames_dir
                   << " (errno=" << errno << ")";
        return false;
    }
    if (out_frames_dir) *out_frames_dir = frames_dir;
    return true;
}

void RemoveDirRecursive(const std::string& dir) {
#ifndef _WIN32
    DIR* d = opendir(dir.c_str());
    if (d == nullptr) return;  // 目录不存在 = 无需清理
    for (dirent* e = readdir(d); e; e = readdir(d)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        const std::string child = dir + "/" + name;
        if (e->d_type == DT_DIR) {
            RemoveDirRecursive(child);
        } else {
            unlink(child.c_str());
        }
    }
    closedir(d);
    rmdir(dir.c_str());
#else
    (void)dir;  // Windows：round_video 目标平台为 Linux/CI，不提供清理
#endif
}

bool RunFfmpeg(const std::string& frame_pattern, const std::string& out_mp4,
               int fps) {
#ifndef _WIN32
    // execvp 需 argv 以 nullptr 结尾；用 vector 自管理内存（各 .c_str() 指向的
    // std::string 在 waitpid 前都存活，无需手动 calloc/free/槽位计数）。
    const std::string crf     = "18";
    const std::string fps_str = std::to_string(fps);
    std::vector<const char*> argv;
    argv.reserve(16);
    argv.push_back("ffmpeg");
    argv.push_back("-y");
    argv.push_back("-framerate");
    argv.push_back(fps_str.c_str());
    argv.push_back("-i");
    argv.push_back(frame_pattern.c_str());
    argv.push_back("-c:v");
    argv.push_back("libx264");
    argv.push_back("-pix_fmt");
    argv.push_back("yuv420p");  // 播放器兼容
    argv.push_back("-crf");
    argv.push_back(crf.c_str());
    argv.push_back("-preset");
    argv.push_back("medium");
    argv.push_back(out_mp4.c_str());
    argv.push_back(nullptr);  // execvp argv 必须以 nullptr 结尾

    const int pid = fork();
    if (pid == 0) {
        // 子进程 exec ffmpeg；execvp 用 PATH 找可执行文件。exec 成功则不复返；
        // 失败（如未装 ffmpeg）才到这，stderr 报错后非 0 退出。
        execvp("ffmpeg", const_cast<char* const*>(argv.data()));
        fprintf(stderr, "execvp ffmpeg 失败（未安装 ffmpeg？）: %s\n",
                strerror(errno));
        _exit(127);
    }
    if (pid < 0) {
        LOG(ERROR) << "fork() 启动 ffmpeg 失败 (errno=" << errno << ")";
        return false;
    }

    int status = 0;
    waitpid(pid, &status, 0);
    const bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!ok) {
        LOG(ERROR) << "ffmpeg 合成视频失败, exit="
                   << (WIFEXITED(status) ? WEXITSTATUS(status) : -1)
                   << ", 输出=" << out_mp4;
    }
    return ok;
#else
    (void)frame_pattern; (void)out_mp4; (void)fps;
    LOG(ERROR) << "RunFfmpeg 暂不支持 Windows（需 _spawn 封装 ffmpeg）";
    return false;
#endif
}

}  // namespace jpov_viewer
