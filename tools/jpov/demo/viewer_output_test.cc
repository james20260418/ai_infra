// viewer_output_test — ResolveOutputTarget / EnsureOutputDir / 临时帧目录 / 递归清理
//
// 这些纯文件/路径工具不依赖 GL / ffmpeg（RunFfmpeg 仅当真的能 spawn 才测，否则跳过），
// 因此可在 cc_test 里用真实临时目录验证，无需 Xvfb。覆盖重构前埋在 RunFourViews/
// RunRoundVideo 里、从未被单测的路径/mkdir/清理逻辑。

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>

#include <glog/logging.h>

#include "tools/jpov/demo/viewer_output.h"

namespace {

// 造一个唯一临时根目录（单测沙盒），测完由测试自行清理。
std::string TempSandbox() {
    const char* tmp = std::getenv("TEST_TMPDIR");
    if (tmp && *tmp) return std::string(tmp) + "/viewer_output_test";
    return "/tmp/viewer_output_test";
}


void ExpectEqStr(const std::string& got, const std::string& want,
                 const char* msg) {
    if (got != want) {
        LOG(FATAL) << msg << ": got '" << got << "', want '" << want << "'";
    }
}

// —— ResolveOutputTarget（纯函数，不进文件系统）——

void TestDirAndBaseFromPath() {
    // 无目录的纯文件名 → dir="."、base 去最后一个后缀。
    jpov_viewer::OutputTarget t = jpov_viewer::ResolveOutputTarget(
        "pliers.gltf", /*explicit_dir*/ "");
    ExpectEqStr(t.dir, ".", "纯文件名应退回 '.' 目录");
    ExpectEqStr(t.base, "pliers", "base 应去掉 .gltf 后缀");
    LOG(INFO) << "OK pure filename";
}

void TestDirFromSubdir() {
    // 带目录 → dir 取该目录。
    jpov_viewer::OutputTarget t = jpov_viewer::ResolveOutputTarget(
        "a/b/pliers.gltf", "");
    ExpectEqStr(t.dir, "a/b", "子目录路径应取所在目录 a/b");
    ExpectEqStr(t.base, "pliers", "base 恒为文件名去后缀");
    LOG(INFO) << "OK subdir path";
}

void TestExplicitOutputDirWins() {
    // --output_dir 非空 → dir 用它（与模型路径无关），base 仍取模型文件名。
    jpov_viewer::OutputTarget t = jpov_viewer::ResolveOutputTarget(
        "/home/x/model.gltf", "/out/renders");
    ExpectEqStr(t.dir, "/out/renders", "显式 output_dir 应覆盖模型所在目录");
    ExpectEqStr(t.base, "model", "base 不受 output_dir 影响");
    LOG(INFO) << "OK explicit output_dir";
}

void TestBasenameWindowsSlashAndDots() {
    // 反斜杠目录（Windows 风格路径在 POSIX 解析器里同样识别为分隔）。
    jpov_viewer::OutputTarget t = jpov_viewer::ResolveOutputTarget(
        "C:\\models\\my.gltf", "");
    ExpectEqStr(t.dir, "C:\\models", "应识别反斜杠作为分隔");
    ExpectEqStr(t.base, "my", "反斜杠路径下的 base 取文件名去后缀");
    // 多后缀（如 .model.gltf）→ base 只去最后一个。
    jpov_viewer::OutputTarget t2 = jpov_viewer::ResolveOutputTarget(
        "x.cat.gltf", "");
    ExpectEqStr(t2.base, "x.cat", "多后缀只去掉最后一个后缀");
    // 无后缀文件 → base 即全名。
    jpov_viewer::OutputTarget t3 = jpov_viewer::ResolveOutputTarget("noext", "");
    ExpectEqStr(t3.base, "noext", "无后缀文件 base=全名");
    LOG(INFO) << "OK windows-slash / multi-dot / no-ext";
}

// —— 文件操作（真实临时目录）——

// 建唯一根沙盒目录（父目录 TEST_TMPDIR 或 /tmp 已存在）。返回根路径；测完由
// 各测试用 RemoveDirRecursive 递归清理（也顺带覆盖清理逻辑）。
std::string MakeSandboxRoot() {
    const std::string root = TempSandbox();
    if (mkdir(root.c_str(), 0755) != 0 && errno != EEXIST) {
        LOG(FATAL) << "无法建测试沙盒根: " << root << " (errno=" << errno << ")";
    }
    return root;
}

void TestEnsureAndTempFramesDirThenCleanup() {
    const std::string root = MakeSandboxRoot();

    // 1) EnsureOutputDir：不存在可建；已存在（幂等）也 true。
    const std::string d1 = root + "/out";
    CHECK(jpov_viewer::EnsureOutputDir(d1)) << "首次建 output_dir 应成功";
    CHECK(jpov_viewer::EnsureOutputDir(d1))
        << "重复 EnsureOutputDir(已存在) 应幂等成功 (EEXIST)";

    // 2) EnsureTempFramesDir：在 d1 下建 "<base>_round_frames" 并回填路径。
    std::string frames;
    CHECK(jpov_viewer::EnsureTempFramesDir(d1, "pliers", &frames))
        << "建临时帧目录应成功";
    ExpectEqStr(frames, d1 + "/pliers_round_frames", "临时帧目录路径拼接");

    // 3) 往里放一个“假帧”文件，验证 RemoveDirRecursive 能整目录递归清掉。
    const std::string fake = frames + "/frame_0000.png";
    { std::ofstream(fake.c_str()) << "fake-png-bytes"; }
    CHECK((std::ifstream(fake.c_str())).good()) << "假帧文件应已写入";

    jpov_viewer::RemoveDirRecursive(frames);
    const int s = ::stat(frames.c_str(), nullptr);
    if (s == 0 || errno != ENOENT) {
        LOG(FATAL) << "RemoveDirRecursive 后临时帧目录应已删除，stat errno=" << errno;
    }
    // 幂等：目录已不在时再删 → 静默成功（不 crash）。
    jpov_viewer::RemoveDirRecursive(frames);

    // 清理沙盒根目录本身。
    jpov_viewer::RemoveDirRecursive(root);
    LOG(INFO) << "OK ensure/tempdir/recursive-cleanup + EEXIST 幂等";
}

}  // namespace

int main(int argc, char** argv) {
    // glog 默认打到 stderr，无需显式 InitGoogleLogging；便于 bazel test 抓日志。
    TestDirAndBaseFromPath();
    TestDirFromSubdir();
    TestExplicitOutputDirWins();
    TestBasenameWindowsSlashAndDots();
    TestEnsureAndTempFramesDirThenCleanup();
    LOG(INFO) << "viewer_output_test 全部通过";
    return 0;
}
