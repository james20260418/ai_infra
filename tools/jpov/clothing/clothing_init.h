// JPOV 穿衣工具 — 后台初始化（glb 加载 + 最近邻三角形匹配器建图）
//
// 职责（2026-09-29 Danis 定）：穿衣工具第一步需要为 glb（人体 reference 与衣服各一份）
// 建立「点到最近三角形」的加速结构（geom::TriangleMatcher3d），让后续「找最近邻三角形」
// 从 O(三角形数) 降到近 O(1)。建图不快（人体面片数万），故把它放在**后台线程**里做，
// 主界面（JPOV 窗口）在完成后台前黑底白字显示进度（"正在..."）。
//
// 2026-09-30 追加：后台加载时**顺带保留衣服的 CPU 几何**（每个 primitive 一份
//   MeshData + 材质），供衣物烘焙（把平移/旋转/缩放烘进顶点）与保存 glb 用（见
//   TakeClothGeometry）。body 侧不保留（不需要）。
//
// 设计边界（重要，别越界）：
//   - 本控制器**只做纯 CPU 工作**：用纯净 loader（LoadGltfScene，GL-free）读 CPU 几何，
//     抽出三角形后建 TriangleMatcher3d。**不碰任何 GL**（GL 资源的上传由主线程在
//     线程完成后用 LoadGltf 做）——这保证后台线程天然线程安全（无需 GL 上下文握手）。
//   - 建图参数（体素边长 grid_size / 查询半径 local_distance）按 Danis 指定取编译期常量
//     （见 kMatcherGridSize / kMatcherLocalDistance）——它们是算法标定量，不做运行期调节。
//
// 线程协议（与 demo/editor/editor_save.h 同款，简单一致）：
//   - Start() 起 worker 线程；worker 只写受 mutex 保护的进度/结果字段；
//   - 主线程每帧 Tick()；完成即 join + 切状态；
//   - 公开方法（Start/Tick/析构）只在主线程调用。
//   - 析构时若仍在跑：置取消标志 → join（wait 很短，见 CancelPollInterval）。

#ifndef JPOV_CLOTHING_CLOTHING_INIT_H_
#define JPOV_CLOTHING_CLOTHING_INIT_H_

#include <atomic>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "geom/3d/triangle_matcher_3d.h"
#include "tools/jpov/clothing/weight_transfer.h"
#include "tools/jpov/interface/mesh.h"
#include "tools/jpov/src/gltf_loader.h"

namespace jpov {
namespace clothing {

// ── 建图参数（编译期常量，Danis 指定）──
// grid_size：体素边长（米）。0.01 = 1cm，与 JPOV 米制一致。
inline constexpr double kMatcherGridSize = 0.01;
// local_distance：查询半径（米）。0.05 = 5cm。
inline constexpr double kMatcherLocalDistance = 0.05;

// 三角形顶点类型别名（与 geom::Triangle3 的标量一致：double）。
using Triangle3d = geom::Triangle3<double>;

// 初始化状态机。
enum class InitState {
    kIdle,       // 未启动（Start 前）
    kBuilding,   // 后台线程在跑；窗口黑底白字显示 progress_message()
    kDone,       // 建图完成：两个匹配器可用（body_matcher()/cloth_matcher()）
    kFailed,     // 失败：error_message() 说明原因
};

// 一个模型（人体 reference 或衣服）的建图结果。
//
// 语义：matcher 建在**资产自身坐标系**（= 未做任何调节时衣服的原始坐标，即 glb 里
//   写死的 y-up 坐标）。⚠️ 注意：衣服面板的平移/旋转/缩放是**烘进渲染 mesh 顶点**的
//   （见 clothing_transform.h），matcher 仍反映**未调节的原始几何**；将来对齐步骤若要用
//   这个 matcher，查询点需先按当前变换反向映射（或届时重建 matcher）。当前（本任务）
//   matcher 尚未被消费。
struct MatcherBundle {
    // TriangleMatcher3d 不能空构造（构造时 CHECK triangles 非空），故用 optional
    // 承载"尚未建成"的空态；valid 与 matcher.has_value() 同义，仅为调用处可读。
    std::optional<geom::TriangleMatcher3d<double>> matcher;

    bool valid() const { return matcher.has_value(); }
};

// 后台初始化控制器（ClothingToolApp 的组合成员；跨帧持有）。
class ClothingInitController {
public:
    ClothingInitController() = default;
    ~ClothingInitController();

    ClothingInitController(const ClothingInitController&) = delete;
    ClothingInitController& operator=(const ClothingInitController&) = delete;

    // 发起后台初始化：读两个 glb 的 CPU 几何 → 抽三角形 → 建 TriangleMatcher3d。
    //
    // body_reference_path：人体 reference 的 glb/gltf 路径。
    // cloth_path        ：衣服模型的 glb/gltf 路径。
    //
    // 返回 false = 未发起（已在跑 / 已在跑过）。返回 true = 已起线程。
    // 路径为空属编程错误（调用方在 CLI 层已校验），此处 CHECK crash。
    bool Start(const std::string& body_reference_path,
               const std::string& cloth_path);

    // 每帧调用：若后台线程已完成则收尾（join + 切状态）。
    // 返回 true 表示本帧状态发生了变化（从 kBuilding → kDone/kFailed）。
    bool Tick();

    InitState state() const { return state_; }

    // 后台进度文案（如 "正在加载人体 reference..."）——窗口黑底白字显示。
    // 线程安全：读时加锁取快照。
    std::string progress_message() const;

    // 失败原因（kFailed 时有效）。
    std::string error_message() const;

    // 建图结果（kDone 后有效）。
    const MatcherBundle& body_matcher() const { return body_; }
    const MatcherBundle& cloth_matcher() const { return cloth_; }

    // 人体「三角形 → 3 角蒙皮」表：与 body_matcher().matcher->triangles() **同序同长**。
    // 供软布自动蒙皮（weight transfer，见 weight_transfer.h）用。
    // ⚠️ 人体 reference 无蒙皮通道（JOINTS_0/WEIGHTS_0）时该表为空（triangle_count()==0）
    //    —— 调用方据此判定「自动蒙皮不可用」，不要当异常崩溃。
    const BodySkinTable& body_skin() const { return body_skin_; }

    // 取走衣服的 CPU 几何（base MeshData + 材质），供衣物变换（就地改顶点，
    // clothing_transform.h）、软体仿真与保存（clothing_save.h）用。kDone 后调用一次；取走后本控制器不再持有（move 语义）。
    // 说明：几何在后台线程加载时**顺带保留**（不额外读一遍 glb）；body 侧不保留。
    std::vector<jpov::GltfMeshEntry> TakeClothGeometry() {
        std::lock_guard<std::mutex> lock(mtx_);
        return std::move(cloth_geometry_);
    }

private:
    void JoinIfRunning();

    InitState state_ = InitState::kIdle;

    // worker 线程与主线程共享的进度/结果字段（受 mtx_ 保护）。
    mutable std::mutex mtx_;
    std::string progress_message_;
    std::string error_message_;
    MatcherBundle body_;
    MatcherBundle cloth_;
    // 人体「三角形 → 3 角蒙皮」表（与 body_ 的三角形同序同长）；无蒙皮时为空。
    BodySkinTable body_skin_;
    // 衣服的 CPU 几何（每个 primitive 一份 MeshData + 材质）；仅衣服侧保留。
    std::vector<jpov::GltfMeshEntry> cloth_geometry_;

    std::thread worker_;
    std::atomic<bool> finished_{false};  // worker 置 true 表示已收工（读结果前先看它）
    std::atomic<bool> cancel_{false};    // 析构时置 true，让 worker 尽快退出
};

}  // namespace clothing
}  // namespace jpov

#endif  // JPOV_CLOTHING_CLOTHING_INIT_H_
