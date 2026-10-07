// JPOV 模型编辑器 — 保存 target 为 glb（后台线程 + 状态机）
//
// 把当前 target 的 CPU 几何（已含平移 / 旋转 / 缩放 / 裁剪的结果）写成一个 glb，落在
// 源 glb 同目录、文件名 <stem>_model_edit<时间戳>.glb。带骨架的 target 一并写 skin。
//
// 设计沿用穿衣工具 clothing_save.h 的模式：
//   - 起一个后台线程写 glb（WriteGlb 是纯 CPU 文件 IO，不碰 GL）；
//   - 线程运行期按钮不可重复触发；完成后面板显示「保存至 <path>」/「保存失败：<原因>」；
//   - 传入的 meshes 是**按下当帧的几何快照**（按值传入），之后 UI 再改不影响本次保存。
//
// 与穿衣工具的差异：本工具在改动时已就地改好 mesh 顶点，故保存时不再施加任何放置变换，
//   直接写当前几何即可。

#ifndef JPOV_MODEL_EDITOR_MODEL_SAVE_H_
#define JPOV_MODEL_EDITOR_MODEL_SAVE_H_

#include <atomic>
#include <ctime>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "tools/jpov/src/gltf_saver.h"

namespace jpov {
namespace model_editor {

// 保存状态机。
enum class ModelSaveState {
    kIdle,     // 空闲：可保存
    kSaving,   // 后台线程在跑：按钮显示「保存中...」，不可重复触发
    kDone,     // 上次保存成功：显示「保存至 <path>」
    kFailed,   // 上次保存失败：显示「保存失败：<原因>」，可重试
};

// 保存控制器（ModelEditorApp 的组合成员；跨帧持有）。
//
// 生命周期：Start 起线程 → Tick 每帧检查完成 → 析构时 join（不 detach）。
class ModelSaveController {
public:
    ModelSaveController() = default;
    ~ModelSaveController() { JoinIfRunning(); }

    ModelSaveController(const ModelSaveController&) = delete;
    ModelSaveController& operator=(const ModelSaveController&) = delete;

    // 发起一次保存。
    //
    // meshes      : 当前 target 几何快照（按值传入，worker 只读）。为空则拒绝（返回 false）。
    // skin        : target 骨架（带骨 target 才有；无骨传 nullopt）。非空时写入 glb 的
    //               skin（含 inverseBindMatrices），前提是各 mesh 带 JOINTS_0/WEIGHTS_0。
    // source_path : 被编辑的 target glb / gltf 路径（取 stem 与同目录定位）。
    // asset_name  : 写入 glb 的模型名（空则写 "model"）。
    //
    // 返回 false：正在保存中 / 没有几何。返回 true：已起线程。
    bool Start(std::vector<jpov::GltfSaveMesh> meshes,
               std::optional<jpov::SkeletonType> skin,
               const std::string& source_path, const std::string& asset_name);

    // 每帧调用：后台线程完成后收尾（join + 切状态 + 组装提示文本）。
    bool Tick();

    ModelSaveState state() const { return state_; }
    const std::string& message() const { return message_; }

    // ---- 纯函数（可单测） ----
    // 由源 glb 路径生成输出路径：同目录 + 原 stem + `_model_edit<时间戳>.glb`。
    // 目录不可推导（无路径分隔符）时退化为当前目录。
    // Pre-condition: source 非空。
    static std::string MakeOutputPath(const std::string& source, std::time_t now);

private:
    void JoinIfRunning();

    ModelSaveState state_ = ModelSaveState::kIdle;
    std::string message_;

    std::thread worker_;
    std::mutex mtx_;
    std::atomic<bool> finished_{false};
    bool ok_ = false;
    std::string out_path_;
    std::string error_;
};

}  // namespace model_editor
}  // namespace jpov

#endif  // JPOV_MODEL_EDITOR_MODEL_SAVE_H_
