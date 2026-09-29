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

// 把一个 primitive 的 MeshData 抽成空间三角形，追加到 out。
//
// 索引网格：按 indices 三三分组；非索引网格：按顶点序每 3 个一组（triangle list 语义，
// 与渲染/仿真的解释一致）。退化（共线/重合/面积趋近 0）的三角形被 Triangle3::Create
// 拒绝（返回 nullopt），静默跳过——退化面片对"最近邻"贡献为零，无须报错。
void AppendTrianglesFromMesh(const MeshData& mesh,
                             std::vector<Triangle3d>* out /*inout*/) {
    CHECK(out != nullptr);
    const std::vector<Vec3f>& pos = mesh.positions;
    if (pos.empty()) {
        return;
    }

    const auto append_one = [&](uint32_t i0, uint32_t i1, uint32_t i2) {
        CHECK_LT(i0, pos.size());
        CHECK_LT(i1, pos.size());
        CHECK_LT(i2, pos.size());
        // MeshData 顶点是 float，本匹配器用 double 标量：显式提升，避免丢精度。
        const geom::Vec3<double> a(pos[i0].x(), pos[i0].y(), pos[i0].z());
        const geom::Vec3<double> b(pos[i1].x(), pos[i1].y(), pos[i1].z());
        const geom::Vec3<double> c(pos[i2].x(), pos[i2].y(), pos[i2].z());
        std::optional<Triangle3d> tri = Triangle3d::Create(a, b, c);
        if (tri.has_value()) {
            out->push_back(tri.value());
        }
    };

    if (!mesh.indices.empty()) {
        CHECK_EQ(mesh.indices.size() % 3, 0u)
            << "索引网格的 indices 必须是 3 的倍数（triangle list）";
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            append_one(mesh.indices[i], mesh.indices[i + 1], mesh.indices[i + 2]);
        }
    } else {
        CHECK_EQ(pos.size() % 3, 0u)
            << "非索引网格的顶点数必须是 3 的倍数（triangle list）";
        for (size_t i = 0; i + 2 < pos.size(); i += 3) {
            append_one(static_cast<uint32_t>(i), static_cast<uint32_t>(i + 1),
                       static_cast<uint32_t>(i + 2));
        }
    }
}

// 回调 user_data：把每个 primitive 的三角形收集进一个 vector。
struct TriangleCollector {
    std::vector<Triangle3d> triangles;
};

void CollectTriangles(const GltfMeshEntry* entry, void* user_data) {
    CHECK(entry != nullptr);
    CHECK(user_data != nullptr);
    TriangleCollector* collector = static_cast<TriangleCollector*>(user_data);
    AppendTrianglesFromMesh(entry->mesh, &collector->triangles);
}

// 读一个 glb 并抽出所有三角形。失败（解析失败）返回 false。
bool LoadTriangles(const std::string& path, std::vector<Triangle3d>* out) {
    CHECK(out != nullptr);
    TriangleCollector collector;
    const bool ok = LoadGltfScene(path, &CollectTriangles, &collector);
    if (!ok) {
        return false;
    }
    *out = std::move(collector.triangles);
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
    }
    state_ = InitState::kBuilding;

    std::thread worker([this, body_reference_path, cloth_path]() {
        std::string error;
        MatcherBundle body;
        MatcherBundle cloth;

        const auto set_progress = [this](const std::string& msg) {
            std::lock_guard<std::mutex> lock(mtx_);
            progress_message_ = msg;
        };

        // 1) 人体 reference：读三角形 → 建匹配器。
        set_progress("正在加载人体 reference...");
        std::vector<Triangle3d> body_triangles;
        if (!LoadTriangles(body_reference_path, &body_triangles)) {
            error = "加载人体 reference 失败：" + body_reference_path;
        } else if (body_triangles.empty()) {
            error = "人体 reference 无有效三角形：" + body_reference_path;
        } else {
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

        // 2) 衣服：同流程（仅在人体步骤成功时继续）。
        if (error.empty() && !cancel_.load()) {
            set_progress("正在加载衣服模型...");
            std::vector<Triangle3d> cloth_triangles;
            if (!LoadTriangles(cloth_path, &cloth_triangles)) {
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
