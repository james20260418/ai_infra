// JPOV 穿衣工具 — 后台初始化实现（glb 加载 + 最近邻三角形匹配器建图）
//
// 见 clothing_init.h 的设计说明。本文件只做：起后台线程 → 读 CPU 几何 → 抽三角形
// → 建 TriangleMatcher3d → 回填进度/结果。全程 GL-free（不碰渲染器）。

#include "tools/jpov/clothing/clothing_init.h"

#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/src/gltf_loader.h"

namespace jpov {
namespace clothing {

namespace {

// 回调 user_data：把每个 primitive 的三角形收集进一个 vector；可选同步建「三角形 → 3 角
// 蒙皮」表（人体侧用）；可选保留一份 CPU 几何（衣服侧用）。
struct PrimitiveCollector {
    std::vector<Triangle3d> triangles;
    // 非空则同步建蒙皮表（见 weight_transfer.h 的 AppendMeshTriangles）。
    BodySkinTable* skin = nullptr;
    // 非空则同时保留 CPU 几何（衣服侧用）。
    std::vector<jpov::GltfMeshEntry>* geometry = nullptr;
};

void CollectPrimitive(const GltfMeshEntry* entry, void* user_data) {
    CHECK(entry != nullptr);
    CHECK(user_data != nullptr);
    PrimitiveCollector* collector = static_cast<PrimitiveCollector*>(user_data);
    // 只对「带蒙皮通道」的 primitive 建表：缺蒙皮的 primitive 会把表与三角形错位，
    // 由调用方在末尾检测（triangle_count != triangles.size()）；此处不 CHECK 崩溃，
    // 让工具在「无蒙皮人体」上仍可用于仿真 / 保存（只是自动蒙皮不可用）。
    BodySkinTable* table =
        (collector->skin != nullptr &&
         MeshHasFlag(entry->mesh.flags, MeshVertexFlags::kJoints))
            ? collector->skin
            : nullptr;
    AppendMeshTriangles(entry->mesh, &collector->triangles, table);
    if (collector->geometry != nullptr) {
        // loader 回调给的是 const 引用，只能拷贝一份（初始化期一次性，可接受）。
        collector->geometry->push_back(*entry);
    }
}

// 读一个 glb 并抽出所有三角形（可选同时保留 CPU 几何 / 建人体蒙皮表）。失败返回 false。
bool LoadModel(const std::string& path, bool keep_geometry,
               std::vector<Triangle3d>* out_triangles,
               std::vector<jpov::GltfMeshEntry>* out_geometry /*output, 可空*/,
               BodySkinTable* out_skin /*output, 可空*/) {
    CHECK(out_triangles != nullptr);
    PrimitiveCollector collector;
    collector.geometry = keep_geometry ? out_geometry : nullptr;
    collector.skin = out_skin;
    const bool ok = LoadGltfScene(path, &CollectPrimitive, &collector);
    if (!ok) {
        return false;
    }
    *out_triangles = std::move(collector.triangles);
    return true;
}

}  // namespace

ClothingInitController::~ClothingInitController() {
    // 让仍在跑（不太可能，构建很快）的 worker 尽快退出，然后 join。
    cancel_.store(true);
    JoinIfRunning();
}

bool ClothingInitController::Start(const std::string& body_reference_path,
                                   const std::string& cloth_path) {
    CHECK(!body_reference_path.empty())
        << "ClothingInitController::Start: body_reference_path 为空（调用方应先校验）";
    CHECK(!cloth_path.empty())
        << "ClothingInitController::Start: cloth_path 为空（调用方应先校验）";
    if (state_ == InitState::kBuilding || state_ == InitState::kDone) {
        LOG(WARNING) << "ClothingInitController::Start 被忽略：已启动过（state="
                     << static_cast<int>(state_) << "）";
        return false;
    }
    JoinIfRunning();  // 回收上一轮（kFailed 重试场景）

    cancel_.store(false);
    finished_.store(false);
    {
        std::lock_guard<std::mutex> lock(mtx_);
        progress_message_ = "正在启动后台初始化...";
        error_message_.clear();
        body_ = MatcherBundle{};
        cloth_ = MatcherBundle{};
        body_skin_ = BodySkinTable{};
        cloth_geometry_.clear();
    }
    state_ = InitState::kBuilding;

    std::thread worker([this, body_reference_path, cloth_path]() {
        std::string error;
        MatcherBundle body;
        MatcherBundle cloth;
        BodySkinTable body_skin;

        const auto set_progress = [this](const std::string& msg) {
            std::lock_guard<std::mutex> lock(mtx_);
            progress_message_ = msg;
        };

        // 1) 人体 reference：读三角形 → 建匹配器；顺带建「三角形 → 3 角蒙皮」表
        //    （供软布自动蒙皮用；人体无蒙皮通道时表为空，自动蒙皮不可用）。
        set_progress("正在加载人体 reference...");
        std::vector<Triangle3d> body_triangles;
        if (!LoadModel(body_reference_path, /*keep_geometry=*/false,
                       &body_triangles, /*out_geometry=*/nullptr,
                       /*out_skin=*/&body_skin)) {
            error = "加载人体 reference 失败：" + body_reference_path;
        } else if (body_triangles.empty()) {
            error = "人体 reference 无有效三角形：" + body_reference_path;
        } else {
            // 蒙皮表必须与三角形同序同长；不等 ⇒ 某些 primitive 缺 JOINTS/WEIGHTS，
            // 清空表（自动蒙皮不可用），但不阻断初始化（仿真 / 保存仍可用）。
            if (body_skin.triangle_count() != body_triangles.size()) {
                LOG(WARNING) << "人体 reference 缺蒙皮信息（JOINTS_0/WEIGHTS_0），"
                                "软布自动蒙皮不可用："
                             << body_reference_path;
                body_skin = BodySkinTable{};
            }
            set_progress("正在为人体 reference 建图（" +
                         std::to_string(body_triangles.size()) + " 面）...");
            const auto t0 = std::chrono::steady_clock::now();
            body.matcher.emplace(kMatcherLocalDistance, kMatcherGridSize,
                                 std::move(body_triangles));
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count();
            LOG(INFO) << "人体 reference 匹配器建图完成："
                      << body.matcher->triangles().size() << " 面，耗时 " << ms
                      << " ms";
        }

        // 2) 衣服：同流程（仅在人体步骤成功时继续），并**保留 CPU 几何**供烘焙/保存。
        std::vector<jpov::GltfMeshEntry> cloth_geometry;
        if (error.empty() && !cancel_.load()) {
            set_progress("正在加载衣服模型...");
            std::vector<Triangle3d> cloth_triangles;
            if (!LoadModel(cloth_path, /*keep_geometry=*/true, &cloth_triangles,
                           &cloth_geometry, /*out_skin=*/nullptr)) {
                error = "加载衣服模型失败：" + cloth_path;
            } else if (cloth_triangles.empty()) {
                error = "衣服模型无有效三角形：" + cloth_path;
            } else {
                set_progress("正在为衣服建图（" +
                             std::to_string(cloth_triangles.size()) + " 面）...");
                const auto t0 = std::chrono::steady_clock::now();
                cloth.matcher.emplace(kMatcherLocalDistance, kMatcherGridSize,
                                      std::move(cloth_triangles));
                const auto ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
                LOG(INFO) << "衣服匹配器建图完成："
                          << cloth.matcher->triangles().size() << " 面，耗时 " << ms
                          << " ms";
            }
        }

        if (cancel_.load()) {
            return;  // 析构中，丢弃结果（主线程不再读）。
        }

        {
            std::lock_guard<std::mutex> lock(mtx_);
            body_ = std::move(body);
            cloth_ = std::move(cloth);
            body_skin_ = std::move(body_skin);
            cloth_geometry_ = std::move(cloth_geometry);
            error_message_ = error;
            progress_message_ = error.empty() ? "初始化完成" : "初始化失败";
        }
        finished_.store(true);
    });
    worker_ = std::move(worker);
    return true;
}

bool ClothingInitController::Tick() {
    if (state_ != InitState::kBuilding) {
        return false;
    }
    if (!finished_.load()) {
        return false;  // 线程还在跑
    }
    JoinIfRunning();

    std::lock_guard<std::mutex> lock(mtx_);
    state_ = error_message_.empty() ? InitState::kDone : InitState::kFailed;
    return true;
}

std::string ClothingInitController::progress_message() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return progress_message_;
}

std::string ClothingInitController::error_message() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return error_message_;
}

void ClothingInitController::JoinIfRunning() {
    if (worker_.joinable()) {
        worker_.join();
    }
}

}  // namespace clothing
}  // namespace jpov
