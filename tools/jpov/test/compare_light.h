// JPOV 光照 Gold Image 比对工具 — 自动平铺 ROI 的平均颜色对比
//
// 背景（leader/Danis 决策方向）：
//   光照 (PBR / 点光源 / llvmpipe 软渲染) 的逐像素比较不可靠 —— 同一场景在
//   Xvfb/Mesa llvmpipe 下会落在多种离散渲染状态之一，逐像素差可达 ±255，
//   固定基准图 + 固定容差无法稳定绿。
//   方案：不做逐像素比对，改为**自动平铺 ROI 的块内平均颜色对比**。
//   小块平均对状态的整块明暗漂移敏感度低（均值摊平了散点差异），
//   但仍能区分"明显变暗/变亮/光被去掉"这类真实回归。
//
// 本工具约定：
//   - ROI 由调用方指定行列数 (nrow × ncol) 自动平铺整张图，无需手填坐标。
//   - 每个 ROI 内：只对不透明 (alpha>0) 像素求 RGB 均值（透明背景不计入）。
//   - 输出：每个 ROI 的 RGB 均值最大通道差 + 全图所有 ROI 中的最大通道差
//     （即 Danis 说的 "最大的 RGB 通道差异"）。
//   - 纯 CPU，无新依赖，只依赖 stb_image（调用方已 link）。

#ifndef JPOV_TEST_COMPARE_LIGHT_H_
#define JPOV_TEST_COMPARE_LIGHT_H_

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>

#include <glog/logging.h>
#include "third_party/stb/stb_image.h"

namespace jpov {

// 一块 ROI 的平均颜色比对结果。
struct RoiMeanDiff {
    int row = 0;         // 平铺行号
    int col = 0;         // 平铺列号
    int x0 = 0, y0 = 0;  // ROI 像素范围 [x0,x1)/[y0,y1)
    int x1 = 0, y1 = 0;
    long long opaque = 0;   // 不透明像素数（0 表示空 ROI，跳过）
    double dr = 0.0, dg = 0.0, db = 0.0;  // 每通道均值差
    double max_channel_diff = 0.0;        // max(|dr|,|dg|,|db|)
};

// 整张图的平均颜色平铺比对结果。
struct MeanCompareReport {
    int width = 0, height = 0;   // 两张图尺寸（必须一致）
    int nrow = 0, ncol = 0;      // 平铺配置
    std::vector<RoiMeanDiff> rois;
    double overall_max_channel_diff = 0.0;  // 所有 ROI 中的最大通道差
    // 最差的那个 ROI（用于日志定位）。
    int worst_roi_index = -1;
};

// 对 gold 与 rendered 两张 RGBA8 图做自动平铺 ROI 的平均颜色对比。
// 返回 overall_max_channel_diff（Danis 说的"最大的 RGB 通道差异"）。
// 全程 LOG(INFO) 把每个 ROI 的均值差打出来；空 ROI（全透明）跳过并记 0。
inline double CompareLightMeanRoi(
    const unsigned char* gold,   // RGBA8，尺寸由 w/h 给出
    const unsigned char* rend,
    int w, int h,
    int nrow, int ncol,
    MeanCompareReport* report_out = nullptr) {
    CHECK(gold != nullptr);
    CHECK(rend != nullptr);
    CHECK_GT(w, 0);
    CHECK_GT(h, 0);
    CHECK_GT(nrow, 0);
    CHECK_GT(ncol, 0);

    MeanCompareReport rep;
    rep.width = w; rep.height = h; rep.nrow = nrow; rep.ncol = ncol;
    rep.rois.reserve(nrow * ncol);

    for (int r = 0; r < nrow; ++r) {
        for (int c = 0; c < ncol; ++c) {
            int y0 = r * h / nrow, y1 = (r + 1) * h / nrow;
            int x0 = c * w / ncol, x1 = (c + 1) * w / ncol;

            RoiMeanDiff roi;
            roi.row = r; roi.col = c;
            roi.x0 = x0; roi.y0 = y0; roi.x1 = x1; roi.y1 = y1;

            double sr = 0, sg = 0, sb = 0;  // rendered 通道累加
            double gr = 0, gg = 0, gb = 0;  // gold 通道累加
            long long n = 0;

            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    const unsigned char* gp = gold + (y * w + x) * 4;
                    const unsigned char* rp = rend + (y * w + x) * 4;
                    // 只统计两张图都不透明的像素（排除背景/图外区域）
                    if (gp[3] == 0 || rp[3] == 0) continue;
                    gr += gp[0]; gg += gp[1]; gb += gp[2];
                    sr += rp[0]; sg += rp[1]; sb += rp[2];
                    ++n;
                }
            }

            roi.opaque = n;
            if (n > 0) {
                roi.dr = ::fabs(sr / n - gr / n);
                roi.dg = ::fabs(sg / n - gg / n);
                roi.db = ::fabs(sb / n - gb / n);
                roi.max_channel_diff =
                    std::max(roi.dr, std::max(roi.dg, roi.db));
                if (roi.max_channel_diff > rep.overall_max_channel_diff) {
                    rep.overall_max_channel_diff = roi.max_channel_diff;
                    rep.worst_roi_index = static_cast<int>(rep.rois.size());
                }
            }
            rep.rois.push_back(roi);
        }
    }

    // 日志输出每个 ROI 均值差（便于肉眼/CI 排查）。
    LOG(INFO) << "CompareLightMeanRoi: tile=" << nrow << "x" << ncol
              << " size=" << w << "x" << h
              << " overall_max_channel_diff=" << rep.overall_max_channel_diff;
    for (size_t i = 0; i < rep.rois.size(); ++i) {
        const RoiMeanDiff& roi = rep.rois[i];
        if (roi.opaque == 0) continue;
        LOG(INFO) << "  ROI[" << i << "] (r=" << roi.row << ",c=" << roi.col
                  << ",px=" << roi.x0 << "," << roi.y0 << "->"
                  << roi.x1 << "," << roi.y1 << ",opaque=" << roi.opaque
                  << ") mean-diff dR=" << roi.dr
                  << " dG=" << roi.dg << " dB=" << roi.db
                  << " |maxChannel=" << roi.max_channel_diff << "|";
    }

    if (report_out) *report_out = rep;
    return rep.overall_max_channel_diff;
}

// 便捷入口：从两张 PNG 路径读图（RGBA8），自动平铺对比，返回最大通道差。
// 尺寸不一致会自动 LOG(ERROR) 并返回 -1（不等尺寸无法逐块对齐）。
inline double CompareLightMeanRoiPng(
    const std::string& gold_path,
    const std::string& rend_path,
    int nrow, int ncol,
    MeanCompareReport* report_out = nullptr) {
    int gw = 0, gh = 0, gn = 0;
    int rw = 0, rh = 0, rn = 0;
    unsigned char* gold = stbi_load(gold_path.c_str(), &gw, &gh, &gn, 4);
    if (!gold) {
        LOG(ERROR) << "CompareLightMeanRoiPng: failed to load gold: "
                   << gold_path << " (" << stbi_failure_reason() << ")";
        return -1;
    }
    unsigned char* rend = stbi_load(rend_path.c_str(), &rw, &rh, &rn, 4);
    if (!rend) {
        LOG(ERROR) << "CompareLightMeanRoiPng: failed to load rendered: "
                   << rend_path << " (" << stbi_failure_reason() << ")";
        stbi_image_free(gold);
        return -1;
    }
    if (gw != rw || gh != rh) {
        LOG(ERROR) << "CompareLightMeanRoiPng: dimension mismatch gold="
                   << gw << "x" << gh << " rendered=" << rw << "x" << rh;
        stbi_image_free(gold);
        stbi_image_free(rend);
        return -1;
    }
    double ret = CompareLightMeanRoi(gold, rend, gw, gh, nrow, ncol,
                                     report_out);
    stbi_image_free(gold);
    stbi_image_free(rend);
    return ret;
}

}  // namespace jpov

#endif  // JPOV_TEST_COMPARE_LIGHT_H_
