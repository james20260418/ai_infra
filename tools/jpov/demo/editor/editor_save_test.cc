// JPOV 模型编辑器 — 保存编排单测（GL-free）
//
// 覆盖（plan §4）：
//   1. MakeOutputPath：同目录 + 原 stem + `_edit<时间戳>.glb`。
//   2. 状态机：Start → kSaving（msessage="保存中..."）→ Tick 完成后转 kDone，
//      且 message 为 "保存至 <path>"；文件真的存在。
//   3. 幂等保护：kSaving 期间再 Start 被拒（返回 false）—— "不可重复触发"的服务端保证。
//   4. 无资产时 Start 返回 false。
//   5. 失败路径：目标目录不可写 → kFailed + message 前缀 "保存失败："。
//
// 说明：本测试**真的跑线程 + 真的写文件**（写 /tmp），因为"保存"的价值就在落盘。

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/demo/editor/editor_save.h"
#include "tools/jpov/interface/mesh.h"
#include "tools/jpov/interface/skeleton_types.h"

namespace jpov_viewer {

class EditorSaveTest {
public:
    static void Run() {
        TestMakeOutputPath();
        TestSaveSuccessFlow();
        TestRejectWhileSaving();
        TestRejectWithoutAsset();
        TestSaveFailurePath();
    }

private:
    static void ExpectTrue(bool cond, const char* msg) {
        if (!cond) LOG(FATAL) << "FAIL: " << msg;
    }

    static jpov::MeshData MakeTri() {
        jpov::MeshData m;
        m.flags = static_cast<jpov::MeshVertexFlags>(
            static_cast<uint8_t>(jpov::MeshVertexFlags::kPosition) |
            static_cast<uint8_t>(jpov::MeshVertexFlags::kNormal) |
            static_cast<uint8_t>(jpov::MeshVertexFlags::kUV));
        m.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
        m.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
        m.uvs = {{0, 0}, {1, 0}, {0, 1}};
        m.indices = {0, 1, 2};
        m.Validate();
        return m;
    }

    static void Feed(SaveController* s, const std::string& name) {
        std::vector<jpov::GltfSaveMesh> meshes;
        meshes.push_back(jpov::GltfSaveMesh{MakeTri(), jpov::GltfMaterialInfo{}});
        s->SetAsset(std::move(meshes), std::nullopt, name);
    }

    // 轮询直到 Tick 收尾（最多 ~5s），失败即 FATAL（不用 sleep 假等固定时长）。
    static void WaitDone(SaveController* s) {
        for (int i = 0; i < 500; ++i) {
            if (s->Tick()) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        LOG(FATAL) << "FAIL: 保存线程 5s 未完成";
    }

    static bool FileExists(const std::string& p) {
        std::ifstream f(p);
        return f.good();
    }

    // 1. 路径命名（纯函数，给固定 time_t 可复现）。
    static void TestMakeOutputPath() {
        const std::time_t t = 0;
        const std::string p =
            SaveController::MakeOutputPath("/data/models/plier.glb", t);
        ExpectTrue(p.rfind("/data/models/plier_edit", 0) == 0,
                   "输出应落在源文件同目录 + 原 stem");
        ExpectTrue(p.size() > 4 && p.substr(p.size() - 4) == ".glb",
                   "输出后缀应为 .glb");
        // 无目录的裸文件名：退化为当前目录（无路径分隔符前缀）。
        const std::string q = SaveController::MakeOutputPath("m.gltf", t);
        ExpectTrue(q.rfind("m_edit", 0) == 0, "无目录时 stem 应直接跟随");
        LOG(INFO) << "OK TestMakeOutputPath";
    }

    // 2. 成功流程：kSaving → kDone，文件落盘，message 为“保存至 …”。
    static void TestSaveSuccessFlow() {
        SaveController s;
        Feed(&s, "ok");
        ModelPlacement pl;   // 默认：identity 旋转 + scale 1
        const std::string src = "/tmp/jpov_editor_save_test_ok.glb";
        std::remove(src.c_str());

        ExpectTrue(s.Start(src, pl), "首次 Start 应成功");
        ExpectTrue(s.state() == SaveState::kSaving, "Start 后应处于 kSaving");
        ExpectTrue(s.message() == "保存中...", "kSaving 提示应为 保存中...");

        WaitDone(&s);
        ExpectTrue(s.state() == SaveState::kDone, "完成后应转 kDone");
        ExpectTrue(s.message().rfind("保存至 ", 0) == 0,
                   "完成提示应以 保存至 开头");
        // 文件真的存在，且是非空 GLB（以 'glTF' magic 开头）。
        const std::string out = s.message().substr(std::string("保存至 ").size());
        ExpectTrue(FileExists(out), "输出文件应存在");
        std::ifstream f(out, std::ifstream::binary);
        char magic[4] = {0, 0, 0, 0};
        f.read(magic, 4);
        ExpectTrue(magic[0] == 'g' && magic[1] == 'l' && magic[2] == 'T' &&
                       magic[3] == 'F',
                   "输出应为 GLB（magic 'glTF'）");
        f.close();
        std::remove(out.c_str());
        LOG(INFO) << "OK TestSaveSuccessFlow";
    }

    // 3. kSaving 期间再次 Start 必须被拒（不可重复触发）。
    static void TestRejectWhileSaving() {
        SaveController s;
        Feed(&s, "reject");
        ModelPlacement pl;
        const std::string src = "/tmp/jpov_editor_save_test_reject.glb";
        ExpectTrue(s.Start(src, pl), "首次 Start 应成功");
        // 立即再触发（线程多半还在跑）。
        ExpectTrue(!s.Start(src, pl), "🔴 kSaving 期间必须拒绝再次 Start");
        WaitDone(&s);
        const std::string out =
            s.message().substr(std::string("保存至 ").size());
        std::remove(out.c_str());
        LOG(INFO) << "OK TestRejectWhileSaving";
    }

    // 4. 无资产时 Start 返回 false（不崩、不起线程）。
    static void TestRejectWithoutAsset() {
        SaveController s;
        ModelPlacement pl;
        ExpectTrue(!s.Start("/tmp/whatever.glb", pl),
                   "无资产时 Start 应返回 false");
        ExpectTrue(s.state() == SaveState::kIdle, "无资产 Start 后仍应 kIdle");
        LOG(INFO) << "OK TestRejectWithoutAsset";
    }

    // 5. 失败路径：目标目录不可写（不存在的目录）→ kFailed。
    static void TestSaveFailurePath() {
        SaveController s;
        Feed(&s, "fail");
        ModelPlacement pl;
        // 源文件在一个不存在的目录 → MakeOutputPath 产出不可写路径。
        const std::string src =
            "/tmp/jpov_no_such_dir_12345/m.glb";
        ExpectTrue(s.Start(src, pl), "Start 应成功（失败在 worker 内发生）");
        WaitDone(&s);
        ExpectTrue(s.state() == SaveState::kFailed, "不可写路径应转 kFailed");
        ExpectTrue(s.message().rfind("保存失败：", 0) == 0,
                   "失败提示应以 保存失败： 开头");
        LOG(INFO) << "OK TestSaveFailurePath";
    }
};

}  // namespace jpov_viewer

int main(int /*argc*/, char** argv) {
    google::InitGoogleLogging(argv[0]);
    jpov_viewer::EditorSaveTest::Run();
    LOG(INFO) << "editor_save_test: ALL PASS";
    return 0;
}
