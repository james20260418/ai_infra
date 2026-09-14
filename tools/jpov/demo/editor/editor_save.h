// JPOV 模型编辑器 — 保存编排（路径命名 + 后台线程 + 状态机）
//
// 职责（对应 docs/jpov_model_editor_save_plan.md §4）：
//   - 按下保存按钮后：起一个**后台线程**把"已烘放置变换的 CPU 资产"写成 .glb；
//   - 线程运行期按钮文字变 "保存中..."，且**不可重复触发**；
//   - 完成后左上角说明追加一条 "保存至 <path>"（失败则 "保存失败：<原因>"）。
//
// 设计取舍（与 plan §4.2 的差异，说明理由）：
//   - plan 曾设想"渲染线程快照几何"因为担心 CPU 几何只在渲染线程可用；但本仓库的
//     编辑器在启动时**额外用纯 loader 加载了一份 CPU 资产**（GL-free），故保存线程
//     只需读这份不可变快照 → **完全不需要与渲染线程做握手**，线程协议大幅简化。
//   - GL 全程不参与（ApplyPlacementToMesh / ApplyPlacementToSkeleton / WriteGlb 都是
//     纯 CPU）→ 后台线程不碰任何 GL 上下文，天然安全。
//
// 线程安全：SaveController 的公开方法（Start/Tick/ClearFinished）只在主线程调用；
//   worker 只写 `done_`/`error_`/`message_` 这几个受 mutex 保护的字段。

#ifndef JPOV_DEMO_EDITOR_SAVE_H_
#define JPOV_DEMO_EDITOR_SAVE_H_

#include <atomic>
#include <cstddef>
#include <ctime>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/demo/editor/model_placement.h"
#include "tools/jpov/interface/mesh.h"
#include "tools/jpov/interface/skeleton_types.h"
#include "tools/jpov/src/gltf_saver.h"

namespace jpov_viewer {

// 保存状态机。
enum class SaveState {
    kIdle,     // 空闲：可保存
    kSaving,   // 后台线程在跑：按钮显示"保存中..."，不可重复触发
    kDone,     // 上次保存成功：显示"保存至 <path>"
    kFailed,   // 上次保存失败：显示"保存失败：<原因>"，可重试
};

// 编辑器保存控制器（EditorApp 的组合成员；跨帧持有）。
//
// 生命周期：Start 起线程 → Tick 每帧检查完成 → 析构时 join（不 detach）。
class SaveController {
public:
    SaveController() = default;
    ~SaveController() { JoinIfRunning(); }

    SaveController(const SaveController&) = delete;
    SaveController& operator=(const SaveController&) = delete;

    // 设置要保存的资产（启动时由 main 用纯 loader 填一次；GL-free 快照）。
    // 这是 worker 线程读的**不可变**输入，Start 之后到线程结束前不得改写。
    void SetAsset(std::vector<jpov::GltfSaveMesh> meshes,
                  std::optional<jpov::SkeletonType> skin,
                  const std::string& asset_name) {
        CHECK(meshes_.empty() || state_ == SaveState::kIdle)
            << "SaveController: SetAsset 只应在空闲时调用一次";
        meshes_ = std::move(meshes);
        skin_ = std::move(skin);
        asset_name_ = asset_name;
    }

    bool has_asset() const { return !meshes_.empty(); }
    size_t primitive_count() const { return meshes_.size(); }

    // 发起一次保存。
    //
    // source_gltf_path : 被编辑的原始 glb/gltf 路径（用于取 stem 与同目录定位）。
    // placement        : 当前放置参数（**在按下当帧拷贝**，之后 UI 变化不影响本次）。
    //
    // 返回 false 表示本次未发起（正在保存中 / 无资产）。返回 true 表示已起线程。
    bool Start(const std::string& source_gltf_path,
               const ModelPlacement& placement);

    // 每帧调用：若后台线程已完成则收尾（join + 切状态 + 组装提示文本）。
    // 返回 true 表示本帧状态发生了变化（调用方可据此重绘/记录）。
    bool Tick();

    SaveState state() const { return state_; }

    // 状态提示（跨帧有效；kDone 时 = "保存至 <path>"，kFailed 时 = "保存失败：<原因>"）。
    const std::string& message() const { return message_; }

    // 重置回空闲（供"再次保存"前清提示，可选）。
    void ResetToIdle() {
        if (state_ == SaveState::kSaving) {
            return;   // 正在跑不动
        }
        state_ = SaveState::kIdle;
        message_.clear();
    }

    // ---- 纯函数（可单测，无线程） ----

    // 由源 glb 路径生成输出路径：同目录 + 原 stem + `_edit<时间戳>.glb`。
    // 目录不可推导（无路径分隔符）时退化为当前目录。
    // Pre-condition: source 非空。
    static std::string MakeOutputPath(const std::string& source,
                                      std::time_t now);

private:
    void JoinIfRunning() {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    // 资产快照（只读输入）。
    std::vector<jpov::GltfSaveMesh> meshes_;
    std::optional<jpov::SkeletonType> skin_;
    std::string asset_name_;

    SaveState state_ = SaveState::kIdle;
    std::string message_;

    std::thread worker_;
    std::mutex mtx_;
    std::atomic<bool> finished_{false};   // worker 置 true 表示已收工
    bool ok_ = false;                     // 本次是否成功（finished_ 后读）
    std::string out_path_;                // 目标路径
    std::string error_;                   // 失败原因（ok_ == false 时有效）
};

}  // namespace jpov_viewer

#endif  // JPOV_DEMO_EDITOR_SAVE_H_
