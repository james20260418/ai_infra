// JPOV 模型编辑器 — 保存 glb 实现（路径命名 + 后台线程 + 状态机）
//
// 见 model_save.h 的设计说明。本文件只做：拼路径、起线程、WriteGlb、回填状态。
// 全程 GL-free（不碰渲染器）。

#include "tools/jpov/model_editor/model_save.h"

#include <exception>
#include <string>
#include <utility>

#include <glog/logging.h>

#include "tools/common/utils.h"

namespace jpov {
namespace model_editor {

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

// 去扩展名。
std::string StemOf(const std::string& base) {
    const size_t dot = base.find_last_of('.');
    if (dot == std::string::npos || dot == 0) {
        return base;
    }
    return base.substr(0, dot);
}

}  // namespace

std::string ModelSaveController::MakeOutputPath(const std::string& source,
                                                std::time_t now) {
    CHECK(!source.empty()) << "ModelSaveController::MakeOutputPath: 路径为空";
    const std::string dir = DirNameOf(source);
    const std::string stem = StemOf(BaseNameOf(source));
    return dir + stem + "_model_edit" + jpov::EditTimestampSuffix(now) + ".glb";
}

bool ModelSaveController::Start(std::vector<jpov::GltfSaveMesh> meshes,
                                std::optional<jpov::SkeletonType> skin,
                                const std::string& source_path,
                                const std::string& asset_name) {
    if (state_ == ModelSaveState::kSaving) {
        LOG(WARNING) << "ModelSaveController::Start 被忽略：正在保存中";
        return false;
    }
    if (meshes.empty()) {
        LOG(ERROR) << "ModelSaveController::Start：没有可保存的几何";
        return false;
    }
    CHECK(!source_path.empty()) << "ModelSaveController::Start：source_path 为空";
    JoinIfRunning();  // 回收上一轮线程（已 finished）

    out_path_ = MakeOutputPath(source_path, std::time(nullptr));
    error_.clear();
    ok_ = false;
    finished_.store(false);
    state_ = ModelSaveState::kSaving;
    message_ = "保存中...";

    // worker 只读 meshes（按值捕获，随线程移动），写受 mtx_ 保护的结果字段。
    std::thread worker([this, meshes = std::move(meshes), skin = std::move(skin),
                        asset_name]() mutable {
        bool ok = true;
        std::string err;
        try {
            jpov::GltfSaveAsset asset;
            asset.name = asset_name.empty() ? "model" : asset_name;
            asset.meshes = std::move(meshes);  // 已是「改好」的几何，直接写
            asset.skin = std::move(skin);      // 无骨 = nullopt（不写 skin）
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
    worker_ = std::move(worker);
    return true;
}

bool ModelSaveController::Tick() {
    if (state_ != ModelSaveState::kSaving) {
        return false;
    }
    if (!finished_.load()) {
        return false;  // 线程还在跑
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
        state_ = ModelSaveState::kDone;
        message_ = "保存至 " + out_path_;
    } else {
        state_ = ModelSaveState::kFailed;
        message_ = "保存失败：" + (err.empty() ? std::string("未知原因") : err);
    }
    return true;
}

void ModelSaveController::JoinIfRunning() {
    if (worker_.joinable()) {
        worker_.join();
    }
}

}  // namespace model_editor
}  // namespace jpov
