// JPOV Mocap — track / pose / 批量报告的 JSON 读写（GL-free）。
//
// 文件格式与 Python 侧保持兼容：
//   - track.json 由 extract/pose_extract.py 产出（本文件只读）；
//   - poses.json 的字段与 fit/mocap_fit.py 输出一致，可直接喂给
//     fit/export_fbx_ascii.py（这样 C++ 拟合与既有 FBX 产出链不用同时改）。
//
// 读文件失败一律返回 false + LOG(ERROR)，不抛异常、不静默产出空序列。

#ifndef JPOV_MOCAP_ALGO_MOCAP_IO_H_
#define JPOV_MOCAP_ALGO_MOCAP_IO_H_

#include <string>
#include <vector>

#include "tools/jpov/mocap/algo/mocap_types.h"

namespace jpov {
namespace mocap {

bool LoadMocapTrack(const std::string& path, MocapTrack* track /*output*/);

// 单条结果：poses.json（字段与 Python fit/mocap_fit.py 对齐）。
bool WritePosesJson(const std::string& path, const DirectionSequence& directions,
                     const FitSequence& fit, const FitMetrics& metrics);

// 批量比对报告的一行。
struct BatchResult {
    std::string track_path;
    std::string algorithm;
    int frame_count = 0;
    float px_per_meter = 0.0f;
    FitMetrics metrics;
};

// 批量报告：JSON（机器读）+ CSV（Excel/肉眼）两份。
bool WriteBatchReportJson(const std::string& path,
                          const std::vector<BatchResult>& results);
bool WriteBatchReportCsv(const std::string& path,
                         const std::vector<BatchResult>& results);

}  // namespace mocap
}  // namespace jpov

#endif  // JPOV_MOCAP_ALGO_MOCAP_IO_H_
