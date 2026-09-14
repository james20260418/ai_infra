// JPOV 模型编辑器 — 保存编排实现（路径命名 + 后台线程 + 状态机）
//
// 见 editor_save.h 的设计说明。本文件只做：拼路径、起线程、烘放置变换、写 glb、
// 回填状态。全程 GL-free（不碰渲染器）。

// ⚠️ MinGW(Windows) 下 M_PI/M_PI_2 须在**首次 include <cmath> 前**定义
// _USE_MATH_DEFINES 才生效：下面经 mesh_transform.h → geom/math_util.h 用到 M_PI，
// 若被系统头先把 <cmath> 拉进来就晚了。与 render_command.h / skeleton_manager.cc
// 同款保护（PR #90 踩过同一个坑）。
#ifdef _WIN32
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <cmath>
#endif

#include "tools/jpov/demo/editor/editor_save.h"

#include <string>
#include <utility>
#include <vector>

#include "tools/common/utils.h"
#include "tools/jpov/interface/mesh_transform.h"

namespace jpov_viewer {

namespace {

// 取路径的目录部分（含末尾分隔符）；无分隔符返回空串。
std::string DirNameOf(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) {
        return std::string();
    }
    return path.substr(0, slash + 1);
}

// 取路径的文件名（去目录）。
std::string BaseNameOf(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

// 去扩展名（保留前导目录已剥离后的 stem）。
std::string StemOf(const std::string& base) {
    const size_t dot = base.find_last_of('.');
    if (dot == std::string::npos || dot == 0) {
        return base;
    }
    return base.substr(0, dot);
}

}  // namespace

std::string SaveController::MakeOutputPath(const std::string& source,
                                           std::time_t now) {
    CHECK(!source.empty()) << "MakeOutputPath: source 路径为空";
    const std::string dir = DirNameOf(source);
    const std::string stem = StemOf(BaseNameOf(source));
    const std::string suffix = jpov::EditTimestampSuffix(now);
    return dir + stem + "_edit" + suffix + ".glb";
}

bool SaveController::Start(const std::string& source_gltf_path,
                           const ModelPlacement& placement) {
    if (state_ == SaveState::kSaving) {
        LOG(WARNING) << "SaveController::Start 被忽略：正在保存中";
        return false;
    }
    if (meshes_.empty()) {
        LOG(ERROR) << "SaveController::Start：没有可保存的资产";
        return false;
    }
    JoinIfRunning();   // 上一轮线程（已 finished）先回收

    const jpov_viewer::DrawPlacement dp = ToDrawParams(placement);
    // 保存时**强制 scale = 1**：骨架烘焙要求刚体 U（见 mesh_transform.h）。
    // 需求验收路径是"旋转保留"；scale 属预览参数，不写进资产（plan §8-3 决策）。
    if (dp.scale != 1.0f) {
        LOG(WARNING) << "保存：忽略预览缩放 scale=" << dp.scale
                     << "（资产按 scale=1 保存；骨架一致性要求刚体放置）";
    }

    out_path_ = MakeOutputPath(source_gltf_path, std::time(nullptr));
    error_.clear();
    ok_ = false;
    finished_.store(false);
    state_ = SaveState::kSaving;
    message_ = "保存中...";

    // worker 读的输入：meshes_/skin_/asset_name_ 是只读快照；dp 按值捕获。
    std::thread t([this, dp]() {
        bool ok = true;
        std::string err;
        try {
            jpov::GltfSaveAsset asset;
            asset.name = asset_name_;
            asset.meshes.reserve(meshes_.size());
            for (const jpov::GltfSaveMesh& sm : meshes_) {
                jpov::GltfSaveMesh baked;
                baked.mesh = jpov::ApplyPlacementToMesh(
                    sm.mesh, dp.center, dp.up, dp.front, /*scale=*/1.0f);
                baked.material = sm.material;
                asset.meshes.push_back(std::move(baked));
            }
            if (skin_.has_value()) {
                asset.skin = jpov::ApplyPlacementToSkeleton(
                    skin_.value(), dp.center, dp.up, dp.front);
            }
            ok = jpov::WriteGlb(asset, out_path_);
            if (!ok) {
                err = "写入失败（见日志）";
            }
        } catch (const std::exception& e) {
            ok = false;
            err = std::string("异常：") + e.what();
        }
        {
            std::lock_guard<std::mutex> lock(mtx_);
            ok_ = ok;
            error_ = err;
        }
        finished_.store(true);
    });
    worker_ = std::move(t);
    return true;
}

bool SaveController::Tick() {
    if (state_ != SaveState::kSaving) {
        return false;
    }
    if (!finished_.load()) {
        return false;   // 线程还在跑
    }
    JoinIfRunning();

    bool ok = false;
    std::string err;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        ok = ok_;
        err = error_;
    }
    if (ok) {
        state_ = SaveState::kDone;
        message_ = "保存至 " + out_path_;
    } else {
        state_ = SaveState::kFailed;
        message_ = "保存失败：" + (err.empty() ? std::string("未知原因") : err);
    }
    return true;
}

}  // namespace jpov_viewer
