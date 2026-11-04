// jpov_mocap_fit — 视频动捕拟合 CLI（C++ 版算法核心的入口）。
//
// 用途（第一期）：
//   1. 读 extract/pose_extract.py 产出的 track.json；
//   2. 用指定的方向场算法（worldz / worldz_t / lift2d / lift2d_t）跑 FitFromDirections；
//   3. 输出与 Python 侧同格式的 poses.json（可直接交给 export_fbx_ascii.py）；
//   4. 批量模式：多 track × 多 algorithm，输出 JSON + CSV 对比报告。
//
// 用法（在仓库根目录执行）：
//   ./bazel-bin/tools/jpov/mocap/algo/jpov_mocap_fit
//       --track ../danis/archery_track.json --algo lift2d_t
//       --out ../danis/archery_lift2d_t.json
//
//   ./bazel-bin/tools/jpov/mocap/algo/jpov_mocap_fit
//       --track a.json --track b.json --algos all
//       --out-dir output/mocap_compare --height 1.78
//
// 注意：--out 只能配一条 track + 一个 algo；批量模式必须用 --out-dir。

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/interface/mixamo23_skeleton.h"
#include "tools/jpov/mocap/algo/mocap_algorithms.h"
#include "tools/jpov/mocap/algo/mocap_fit.h"
#include "tools/jpov/mocap/algo/mocap_io.h"

namespace {

struct Cli {
    std::vector<std::string> tracks;
    std::string out;
    std::string out_dir;
    std::string algo = "lift2d_t";
    std::vector<std::string> algos_explicit;
    bool algos_all = false;
    float height = 1.75f;
    bool no_yaw = false;
    bool no_root = false;
    bool no_ground = false;
    int smoothing_window = 5;
};

std::vector<std::string> SplitCsv(const std::string& text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t comma = text.find(',', start);
        const size_t end = (comma == std::string::npos) ? text.size() : comma;
        if (end > start) {
            out.push_back(text.substr(start, end - start));
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return out;
}

void PrintUsage() {
    std::cerr
        << "用法: jpov_mocap_fit --track <track.json> [--track ...] [选项]\n"
        << "  --out <poses.json>      单条输出（只能配一条 track + 一个 algo）\n"
        << "  --out-dir <dir>         批量输出目录（写 <track>_<algo>.json + 报告）\n"
        << "  --algo <name>           单个算法（默认 lift2d_t）\n"
        << "  --algos all|a,b,c       多算法（批量比对）\n"
        << "  --height <m>            被测者骨骼身高（默认 1.75）\n"
        << "  --no-yaw                不解整体朝向\n"
        << "  --no-root               不解根位移\n"
        << "  --no-ground             不把脚钉到 y=0\n"
        << "  --smoothing-window <n>  根位移平滑窗口（默认 5；<=1 关）\n"
        << "  --list-algos            列出已注册算法\n";
}

Cli ParseCli(int argc, char** argv) {
    Cli cli;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need_value = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                LOG(FATAL) << "参数 " << name << " 缺少取值";
            }
            return argv[++i];
        };
        if (arg == "--track") {
            cli.tracks.push_back(need_value("--track"));
        } else if (arg == "--out") {
            cli.out = need_value("--out");
        } else if (arg == "--out-dir") {
            cli.out_dir = need_value("--out-dir");
        } else if (arg == "--algo") {
            cli.algo = need_value("--algo");
        } else if (arg == "--algos") {
            const std::string value = need_value("--algos");
            if (value == "all") {
                cli.algos_all = true;
            } else {
                std::vector<std::string> names = SplitCsv(value);
                CHECK(!names.empty()) << "--algos 不能为空";
                cli.algos_explicit.insert(cli.algos_explicit.end(), names.begin(),
                                          names.end());
            }
        } else if (arg == "--height") {
            cli.height = static_cast<float>(std::atof(need_value("--height").c_str()));
        } else if (arg == "--no-yaw") {
            cli.no_yaw = true;
        } else if (arg == "--no-root") {
            cli.no_root = true;
        } else if (arg == "--no-ground") {
            cli.no_ground = true;
        } else if (arg == "--smoothing-window") {
            cli.smoothing_window = std::atoi(need_value("--smoothing-window").c_str());
        } else if (arg == "--list-algos") {
            for (const jpov::mocap::DirectionAlgorithm& algo :
                 jpov::mocap::ListDirectionAlgorithms()) {
                std::cout << algo.name << "\t" << algo.description << "\n";
            }
            std::exit(0);
        } else if (arg == "--help" || arg == "-h") {
            PrintUsage();
            std::exit(0);
        } else {
            LOG(FATAL) << "未知参数: " << arg;
        }
    }
    return cli;
}

std::string TrackStem(const std::string& path) {
    return std::filesystem::path(path).stem().string();
}

}  // namespace

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);
    const Cli cli = ParseCli(argc, argv);
    CHECK(!cli.tracks.empty()) << "至少给一个 --track";
    CHECK_GT(cli.height, 0.0f) << "--height 必须 >0";
    CHECK(cli.smoothing_window >= 1) << "--smoothing-window 必须 >=1";

    std::vector<std::string> algos;
    if (!cli.algos_explicit.empty()) {
        algos = cli.algos_explicit;
    } else if (cli.algos_all) {
        for (const jpov::mocap::DirectionAlgorithm& algo :
             jpov::mocap::ListDirectionAlgorithms()) {
            algos.push_back(algo.name);
        }
    } else {
        algos.push_back(cli.algo);
    }
    CHECK(!algos.empty()) << "没有选中任何算法";

    const bool batch = cli.tracks.size() > 1 || algos.size() > 1;
    CHECK(!(batch && cli.out_dir.empty()))
        << "多 track / 多算法必须用 --out-dir（--out 只能收单条结果）";
    CHECK(!cli.out_dir.empty() || !cli.out.empty())
        << "需要 --out 或 --out-dir";
    if (!cli.out_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(cli.out_dir, ec);
        CHECK(!ec) << "创建 --out-dir 失败: " << cli.out_dir << " (" << ec.message() << ")";
    }

    std::vector<jpov::mocap::BatchResult> results;
    for (const std::string& track_path : cli.tracks) {
        jpov::mocap::MocapTrack track;
        CHECK(jpov::mocap::LoadMocapTrack(track_path, &track))
            << "读取 track 失败: " << track_path;

        const jpov::SkeletonType skeleton =
            jpov::Mixamo23Skeleton(/*height=*/cli.height);
        for (const std::string& algo_name : algos) {
            const jpov::mocap::DirectionAlgorithm* algo =
                jpov::mocap::FindDirectionAlgorithm(algo_name);
            CHECK(algo != nullptr) << "未知算法: " << algo_name
                                   << "（--list-algos 看已注册列表）";

            jpov::mocap::DirectionSequence directions = algo->compute(track, skeleton);
            CHECK_EQ(directions.frames.size(), track.frames.size())
                << "算法 " << algo_name << " 输出的帧数不对";

            jpov::mocap::FitOptions options;
            options.subject_height_m = cli.height;
            options.solve_body_yaw = !cli.no_yaw;
            options.solve_root_translation = !cli.no_root;
            options.ground_feet = !cli.no_ground;
            options.root_smoothing_window = cli.smoothing_window;

            jpov::mocap::FitSequence fit =
                jpov::mocap::FitFromDirections(skeleton, directions, track, options);
            fit.fps = track.fps;
            const jpov::mocap::FitMetrics metrics =
                jpov::mocap::EvaluateFit(track, skeleton, fit);

            std::string out_path;
            if (!cli.out.empty()) {
                out_path = cli.out;
            } else {
                out_path = (std::filesystem::path(cli.out_dir) /
                            (TrackStem(track_path) + "_" + algo_name + ".json"))
                               .string();
            }
            CHECK(jpov::mocap::WritePosesJson(out_path, directions, fit, metrics))
                << "写输出失败: " << out_path;

            jpov::mocap::BatchResult row;
            row.track_path = track_path;
            row.algorithm = algo_name;
            row.frame_count = static_cast<int>(fit.frames.size());
            row.px_per_meter = directions.px_per_meter;
            row.metrics = metrics;
            results.push_back(row);

            std::cout << "[mocap] " << TrackStem(track_path) << " / " << algo_name
                      << "  frames=" << row.frame_count
                      << "  dir_err=" << metrics.mean_direction_error_deg << "deg"
                      << "  jitter=" << (metrics.jitter_m * 1000.0f) << "mm"
                      << "  root_y=[" << metrics.root_y_min_m << ", "
                      << metrics.root_y_max_m << "]"
                      << "  -> " << out_path << "\n";
        }
    }

    if (!cli.out_dir.empty()) {
        const std::string report_json =
            (std::filesystem::path(cli.out_dir) / "mocap_report.json").string();
        const std::string report_csv =
            (std::filesystem::path(cli.out_dir) / "mocap_report.csv").string();
        CHECK(jpov::mocap::WriteBatchReportJson(report_json, results));
        CHECK(jpov::mocap::WriteBatchReportCsv(report_csv, results));
        std::cout << "[mocap] report: " << report_json << " / " << report_csv << "\n";
    }
    return 0;
}
