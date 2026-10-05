// JPOV 穿衣工具 — 保存衣服 mesh 为 glb（后台线程 + 状态机）
//
// 需求（2026-09-30 Danis 定）：面板加一个"保存衣服"按钮，把**当前已变换好的**衣服
//   mesh 存成一个 glb，便于把调好的结果带走 / 交给后续管线。
//
// 设计（沿用 demo/editor/editor_save.h 的保存模式）：
//   - 按下按钮后起一个**后台线程**写 glb（WriteGlb 是纯 CPU 文件 IO，不碰 GL）；
//   - 线程运行期按钮不可重复触发；完成后面板显示 "保存至 <path>" / "保存失败：<原因>"。
//   - 传入的 meshes 是**按下当帧的几何快照**（按值传入），之后 UI 再改衣服不影响本次保存。
//
// 与 editor_save 的差异：本工具在**改动时就已就地改好 mesh 顶点**（见
//   clothing_transform.h），故保存时**不再施加任何放置变换**，直接写当前几何即可。

#ifndef JPOV_CLOTHING_CLOTHING_SAVE_H_
#define JPOV_CLOTHING_CLOTHING_SAVE_H_

#include <atomic>
#include <ctime>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "tools/jpov/src/gltf_saver.h"

namespace jpov {
namespace clothing {

// 保存状态机。
enum class ClothSaveState {
    kIdle,     // 空闲：可保存
    kSaving,   // 后台线程在跑：按钮显示"保存中..."，不可重复触发
    kDone,     // 上次保存成功：显示"保存至 <path>"
    kFailed,   // 上次保存失败：显示"保存失败：<原因>"，可重试
};

// 衣服保存控制器（ClothingToolApp 的组合成员；跨帧持有）。
//
// 生命周期：Start 起线程 → Tick 每帧检查完成 → 析构时 join（不 detach）。
class ClothingSaveController {
public:
    ClothingSaveController() = default;
    ~ClothingSaveController() { JoinIfRunning(); }

    ClothingSaveController(const ClothingSaveController&) = delete;
    ClothingSaveController& operator=(const ClothingSaveController&) = delete;

    // 发起一次保存。
    //
    // meshes      : 当前衣服几何快照（按值传入，worker 只读）。为空则拒绝（返回 false）。
    // skin        : 蒙皮骨架（软布自动蒙皮后传入；未蒙皮传 nullopt）。非空时写入 glb 的
    //               skin（含 inverseBindMatrices），前提是各 mesh 带 JOINTS_0/WEIGHTS_0
    //               （即 flags 含 kJoints）——否则写出的 skin 是死的（无顶点引用）。
    // source_path : 被编辑的衣服 glb / gltf 路径（用于取 stem 与同目录定位）。
    // asset_name  : 写入 glb 的模型名（面板显示用；空则写 "cloth"）。
    //
    // 返回 false：正在保存中 / 没有几何。返回 true：已起线程。
    bool Start(std::vector<jpov::GltfSaveMesh> meshes,
               std::optional<jpov::SkeletonType> skin,
               const std::string& source_path, const std::string& asset_name);

    // 每帧调用：后台线程完成后收尾（join + 切状态 + 组装提示文本）。
    // 返回 true 表示本帧状态发生了变化。
    bool Tick();

    ClothSaveState state() const { return state_; }

    // 状态提示（跨帧有效）。
    const std::string& message() const { return message_; }

    // ---- 纯函数（可单测） ----
    // 由源 glb 路径生成输出路径：同目录 + 原 stem + `_cloth_edit<时间戳>.glb`。
    // 目录不可推导（无路径分隔符）时退化为当前目录。
    // Pre-condition: source 非空。
    static std::string MakeOutputPath(const std::string& source,
                                      std::time_t now);

private:
    void JoinIfRunning();

    ClothSaveState state_ = ClothSaveState::kIdle;
    std::string message_;

    std::thread worker_;
    std::mutex mtx_;
    std::atomic<bool> finished_{false};  // worker 置 true 表示已收工
    bool ok_ = false;                    // 本次是否成功（finished_ 后读）
    std::string out_path_;               // 目标路径
    std::string error_;                  // 失败原因（ok_ == false 时有效）
};

}  // namespace clothing
}  // namespace jpov

#endif  // JPOV_CLOTHING_CLOTHING_SAVE_H_
