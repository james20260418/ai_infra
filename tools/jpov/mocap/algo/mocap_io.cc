// JPOV Mocap — JSON 读写实现。

#include "tools/jpov/mocap/algo/mocap_io.h"

#include <fstream>
#include <iomanip>
#include <sstream>

#include <glog/logging.h>
#include <nlohmann/json.hpp>

#include "tools/jpov/interface/mixamo23_skeleton.h"

namespace jpov {
namespace mocap {
namespace {

using nlohmann::json;

bool ReadJsonFile(const std::string& path, json* out) {
    CHECK(out != nullptr) << "ReadJsonFile: out 不能为空";
    std::ifstream in(path);
    if (!in.is_open()) {
        LOG(ERROR) << "mocap: 打不开 JSON 文件: " << path;
        return false;
    }
    try {
        in >> *out;
    } catch (const std::exception& e) {
        LOG(ERROR) << "mocap: JSON 解析失败 (" << path << "): " << e.what();
        return false;
    }
    return true;
}

bool ReadLandmark2D(const json& item, Landmark2D* out) {
    CHECK(out != nullptr);
    if (!item.is_object() || !item.contains("x") || !item.contains("y")) {
        return false;
    }
    out->x_px = item.value("x", 0.0f);
    // 提取器是图像坐标（y 向下）；本层统一翻成 y-up。
    out->y_px = -item.value("y", 0.0f);
    out->z_rel = item.value("z", 0.0f);
    out->visibility = item.value("visibility", 1.0f);
    return std::isfinite(out->x_px) && std::isfinite(out->y_px);
}

bool ReadLandmark3D(const json& item, Vec3f* out) {
    CHECK(out != nullptr);
    if (!item.is_object() || !item.contains("x") || !item.contains("y") ||
        !item.contains("z")) {
        return false;
    }
    const float x = item.value("x", 0.0f);
    const float y = item.value("y", 0.0f);
    const float z = item.value("z", 0.0f);
    // MediaPipe world：y 向下、z **越小越靠近相机**；本工程的观察世界是 Y-up、
    // +Z 朝相机。故 y 与 z 都要翻号（只翻 y 会让角色面朝反方向，实测 yaw 变成 170°）。
    *out = Vec3f{x, -y, -z};
    return out->IsFinite();
}

std::string CsvEscape(const std::string& value) {
    if (value.find_first_of(",\"\n") == std::string::npos) {
        return value;
    }
    std::string out = "\"";
    for (char c : value) {
        if (c == '"') {
            out += "\"\"";
        } else {
            out += c;
        }
    }
    out += "\"";
    return out;
}

}  // namespace

bool LoadMocapTrack(const std::string& path, MocapTrack* track) {
    CHECK(track != nullptr) << "LoadMocapTrack: track 不能为空";
    json root;
    if (!ReadJsonFile(path, &root)) {
        return false;
    }
    if (!root.is_object() || !root.contains("frames") || !root["frames"].is_array()) {
        LOG(ERROR) << "mocap: track 缺 frames 数组: " << path;
        return false;
    }
    MocapTrack out;
    out.version = root.value("version", 0);
    out.fps = root.value("fps", 0.0);
    out.width = root.value("width", 0);
    out.height = root.value("height", 0);
    out.source = root.value("source", std::string());
    if (!(out.fps > 0.0)) {
        LOG(ERROR) << "mocap: track.fps 必须 >0，收到 " << out.fps;
        return false;
    }
    const json& frames = root["frames"];
    out.frames.reserve(frames.size());
    for (size_t i = 0; i < frames.size(); ++i) {
        const json& item = frames[i];
        if (!item.is_object()) {
            LOG(ERROR) << "mocap: frames[" << i << "] 不是对象";
            return false;
        }
        TrackFrame frame;
        frame.t_seconds = item.value("t", static_cast<double>(i) / out.fps);
        frame.presence = item.value("presence", 0.0f);

        const json& landmarks = item.contains("landmarks") ? item["landmarks"] : json::array();
        if (landmarks.is_array() && landmarks.size() == kLandmarkCount) {
            bool ok = true;
            for (int k = 0; k < kLandmarkCount; ++k) {
                if (!ReadLandmark2D(landmarks[static_cast<size_t>(k)],
                                    &frame.landmarks[static_cast<size_t>(k)])) {
                    ok = false;
                    break;
                }
            }
            frame.has_landmarks = ok;
        }
        const json& world = item.contains("world") ? item["world"] : json::array();
        if (world.is_array() && world.size() == kLandmarkCount) {
            bool ok = true;
            for (int k = 0; k < kLandmarkCount; ++k) {
                if (!ReadLandmark3D(world[static_cast<size_t>(k)],
                                    &frame.world[static_cast<size_t>(k)])) {
                    ok = false;
                    break;
                }
            }
            frame.has_world = ok;
        }
        if (!frame.has_landmarks && !frame.has_world) {
            LOG(WARNING) << "mocap: frames[" << i << "] 既无 2D 也无 world，按空帧处理";
        }
        out.frames.push_back(frame);
    }
    if (out.frames.empty()) {
        LOG(ERROR) << "mocap: track 没有任何帧: " << path;
        return false;
    }
    *track = std::move(out);
    return true;
}

bool WritePosesJson(const std::string& path, const DirectionSequence& directions,
                    const FitSequence& fit, const FitMetrics& metrics) {
    CHECK_GT(fit.fps, 0.0) << "WritePosesJson: fit.fps 必须 >0（调用方忘了填）";
    json root;
    root["algo"] = directions.algorithm_name;
    root["fps"] = fit.fps;
    root["bone_count"] = kMixamo23BoneCount;
    root["height"] = fit.subject_height_m;
    root["scale"] = fit.subject_height_m / kMixamoOfficialBoneHeight;
    root["metrics"] = {
        {"dir_err_deg", metrics.mean_direction_error_deg},
        {"jitter_m", metrics.jitter_m},
        {"root_y_min_m", metrics.root_y_min_m},
        {"root_y_max_m", metrics.root_y_max_m},
        {"max_foot_penetration_m", metrics.max_foot_penetration_m},
        {"evaluated_bones", metrics.evaluated_bones},
        {"px_per_meter", directions.px_per_meter},
    };
    json frames = json::array();
    for (size_t i = 0; i < fit.frames.size(); ++i) {
        const FitFrame& frame = fit.frames[i];
        CHECK_EQ(frame.pose.joint_rotation.size(), static_cast<size_t>(kMixamo23BoneCount))
            << "WritePosesJson: pose != 23 骨";
        json quat = json::array();
        for (const geom::Quaternion<float>& q : frame.pose.joint_rotation) {
            quat.push_back({q.x, q.y, q.z, q.w});
        }
        json root_entry = {
            {"t", static_cast<double>(i) / fit.fps},
            {"quat", quat},
            {"root", {frame.pose.root_offset.x(), frame.pose.root_offset.y(),
                      frame.pose.root_offset.z()}},
            {"yaw_deg", frame.body_yaw_rad * 180.0f / 3.14159265358979323846f},
            {"foot_clearance_m", frame.foot_clearance_m},
        };
        frames.push_back(std::move(root_entry));
    }
    root["frames"] = std::move(frames);

    std::ofstream out(path);
    if (!out.is_open()) {
        LOG(ERROR) << "mocap: 写不了 poses 文件: " << path;
        return false;
    }
    out << std::setprecision(9) << root.dump(1) << "\n";
    return out.good();
}

bool WriteBatchReportJson(const std::string& path,
                          const std::vector<BatchResult>& results) {
    json root;
    root["version"] = 1;
    json rows = json::array();
    for (const BatchResult& r : results) {
        rows.push_back({
            {"track", r.track_path},
            {"algo", r.algorithm},
            {"frames", r.frame_count},
            {"px_per_meter", r.px_per_meter},
            {"dir_err_deg", r.metrics.mean_direction_error_deg},
            {"jitter_m", r.metrics.jitter_m},
            {"root_y_min_m", r.metrics.root_y_min_m},
            {"root_y_max_m", r.metrics.root_y_max_m},
            {"max_foot_penetration_m", r.metrics.max_foot_penetration_m},
            {"evaluated_bones", r.metrics.evaluated_bones},
        });
    }
    root["results"] = std::move(rows);
    std::ofstream out(path);
    if (!out.is_open()) {
        LOG(ERROR) << "mocap: 写不了批量报告: " << path;
        return false;
    }
    out << std::setprecision(9) << root.dump(1) << "\n";
    return out.good();
}

bool WriteBatchReportCsv(const std::string& path,
                         const std::vector<BatchResult>& results) {
    std::ofstream out(path);
    if (!out.is_open()) {
        LOG(ERROR) << "mocap: 写不了 CSV 报告: " << path;
        return false;
    }
    out << "track,algo,frames,px_per_meter,dir_err_deg,jitter_mm,"
           "root_y_min_m,root_y_max_m,foot_penetration_mm,evaluated_bones\n";
    for (const BatchResult& r : results) {
        out << CsvEscape(r.track_path) << ',' << CsvEscape(r.algorithm) << ','
            << r.frame_count << ',' << r.px_per_meter << ','
            << r.metrics.mean_direction_error_deg << ','
            << (r.metrics.jitter_m * 1000.0f) << ','
            << r.metrics.root_y_min_m << ',' << r.metrics.root_y_max_m << ','
            << (r.metrics.max_foot_penetration_m * 1000.0f) << ','
            << r.metrics.evaluated_bones << '\n';
    }
    return out.good();
}

}  // namespace mocap
}  // namespace jpov
