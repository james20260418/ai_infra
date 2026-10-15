// JPOV 覆盖率保持（coverage-preserving）alpha mip 链生成 —— GL-free 纯函数。
//
// 背景（要解决的坑）：
//   alpha cutout（glTF alphaMode=MASK）的片元保留判据是「采样 alpha >= uAlphaCutoff」。
//   而 glGenerateMipmap 对 RGBA 做线性盒式平均 —— 叶卡贴图里大量透明背景会把 alpha
//   平均到 cutoff 以下，于是「阈值裁剪后仍可见的纹素占比（覆盖率）」随 mip 层级逐级
//   缩水：远景时叶卡变稀、甚至整片消失。这是 alpha-test foliage 的经典问题。
//
// 解法（在 mip 生成侧修正 alpha）：
//   颜色通道照常盒式平均；alpha 通道逐级「按覆盖率反解一个阈值后二值化（0/255）」，
//   使每一级 mip 的覆盖率（alpha>=cutoff 的纹素占比）恒等于 mip0 的覆盖率。
//   这样着色器里那个固定 cutoff 在每一级都命中正确比例 —— 覆盖率不缩水，远景不再变稀。
//
// 约定：
//   - mip0 **原样保留**（近景逐字节零回归；其 alpha 可能带抗锯齿中间值，交原阈值裁剪）。
//   - 覆盖率按 mip0「alpha >= cutoff」的纹素占比定义（与着色器判据一致）。
//   - 逐级二值化时**不会把纯空（alpha==0）纹素变可见**，避免凭空造出斑点。
//   - 层级尺寸按 GL 归约规则 max(1, floor(w / 2^i)) 递减到 1x1，与 glGenerateMipmap
//     生成的层级数一致（避免出现未定义的高层 mip）。

#ifndef JPOV_SRC_COVERAGE_MIPMAP_H_
#define JPOV_SRC_COVERAGE_MIPMAP_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include <glog/logging.h>

namespace jpov {

// 一级 mip：尺寸 + RGBA 像素（width*height*4 字节）。
struct MipLevel {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> rgba;
};

namespace coverage_mipmap_detail {

// 由上一级 src（w x h，RGBA）做 2x2 盒式降采样到 dst（dw x dh，RGBA）。
// 归约规则：dst(x,y) = src[2x..2x+1][2y..2y+1] 的平均（越界取存在的）。四舍五入。
inline void BoxDownsample(const unsigned char* src, int w, int h,
                          unsigned char* dst, int dw, int dh) {
    for (int y = 0; y < dh; ++y) {
        for (int x = 0; x < dw; ++x) {
            int acc[4] = {0, 0, 0, 0};
            int n = 0;
            for (int oy = 0; oy < 2; ++oy) {
                const int sy = 2 * y + oy;
                if (sy >= h) {
                    continue;
                }
                for (int ox = 0; ox < 2; ++ox) {
                    const int sx = 2 * x + ox;
                    if (sx >= w) {
                        continue;
                    }
                    const unsigned char* p = src + (static_cast<size_t>(sy) * w + sx) * 4;
                    acc[0] += p[0];
                    acc[1] += p[1];
                    acc[2] += p[2];
                    acc[3] += p[3];
                    ++n;
                }
            }
            unsigned char* q = dst + (static_cast<size_t>(y) * dw + x) * 4;
            for (int c = 0; c < 4; ++c) {
                q[c] = static_cast<unsigned char>((acc[c] + n / 2) / n);
            }
        }
    }
}

// 确定性空间哈希（用于临界档抖散）：同样 (x,y) 恒得同值，全局铺开无偏。
// 相比 Bayer，避免低覆盖率时选中纹素只在偶数列聚成竖条。
inline uint32_t HashXY(int x, int y) {
    uint32_t h = static_cast<uint32_t>(x) * 73856093u ^
                 static_cast<uint32_t>(y) * 19349663u;
    h ^= h >> 13;
    h *= 0x85ebca6bu;
    h ^= h >> 16;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

// 把整级 alpha 二值化（0 或 255），使「可见（alpha==255）」的纹素数**恰好** =
// target = round(coverage * 纹素数)：按 alpha 从高到低取前 target 个，同 alpha 值（临界档）
// 用空间哈希抖散；**绝不选中纯空（alpha==0）纹素**（target 超过正 alpha 纹素数时
// 退化为「全部正 alpha 纹素保留」）。
// 直方图 + 临界档精确截断 ⇒ |可见数 - coverage*N| ≤ 0.5（而非整档跳变）。
inline void BinarizeAlphaToCoverage(unsigned char* rgba, int w, int h, float coverage) {
    const int n = w * h;
    int target = static_cast<int>(coverage * static_cast<float>(n) + 0.5f);

    int hist[256] = {0};   // hist[v] = alpha==v 的纹素数
    int positive = 0;      // 正 alpha 纹素数（可入选上限，防造空斑点）
    for (int i = 0; i < n; ++i) {
        const int a = rgba[i * 4 + 3];
        hist[a]++;
        if (a > 0) {
            ++positive;
        }
    }
    if (target > positive) {
        target = positive;
    }
    if (target <= 0) {
        for (int i = 0; i < n; ++i) {
            rgba[i * 4 + 3] = 0;
        }
        return;
    }

    // threshold: 保留 alpha > threshold 的全部；alpha == threshold 仅保留前 remain 个。
    int cumulative_above = 0;   // = sum_{u > v} hist[u]
    int threshold = 1;
    for (int v = 255; v >= 1; --v) {
        if (cumulative_above + hist[v] >= target) {
            threshold = v;
            break;
        }
        cumulative_above += hist[v];
    }
    const int remain = target - cumulative_above;   // ∈ [1, hist[threshold]]

    // 第一遍：非临界档直接定值；临界档（alpha==threshold）收集进哈希桶待定。
    std::vector<int> tie_buckets[64];
    for (int i = 0; i < n; ++i) {
        const int a = rgba[i * 4 + 3];
        if (a > threshold) {
            rgba[i * 4 + 3] = 255;
        } else {
            rgba[i * 4 + 3] = 0;
            if (a == threshold) {
                tie_buckets[HashXY(i % w, i / w) & 63u].push_back(i);
            }
        }
    }

    // 第二遍：从最低哈希桶起填 remain 个临界档纹素（∈ [1, hist[threshold]]，必填满）。
    int need = remain;
    for (int b = 0; b < 64 && need > 0; ++b) {
        for (int idx : tie_buckets[b]) {
            if (need <= 0) {
                break;
            }
            rgba[idx * 4 + 3] = 255;
            --need;
        }
    }
}

}  // namespace coverage_mipmap_detail

// 生成覆盖率保持的 alpha mip 链（见文件头）。
//
// 返回 level0..N：level0 = 入参原样拷贝；之后每级为上一级的 2x2 盒式降采样（颜色），
// 但 alpha 通道被二值化以保持 mip0 覆盖率。层级一路减半到 1x1。
//
// Pre-condition: rgba 指向 width*height*4 字节；width>0；height>0；0 < cutoff < 1。
inline std::vector<MipLevel> BuildCoverageMipChain(const unsigned char* rgba,
                                                   int width, int height,
                                                   float cutoff) {
    CHECK(rgba != nullptr) << "BuildCoverageMipChain: rgba 为空";
    CHECK_GT(width, 0);
    CHECK_GT(height, 0);
    CHECK_GT(cutoff, 0.0f) << "BuildCoverageMipChain: cutoff 必须 > 0";
    CHECK_LT(cutoff, 1.0f) << "BuildCoverageMipChain: cutoff 必须 < 1";

    std::vector<MipLevel> levels;
    levels.push_back(MipLevel{
        width, height,
        std::vector<unsigned char>(rgba,
                                   rgba + static_cast<size_t>(width) * height * 4)});

    // mip0 覆盖率：alpha（归一化后）>= cutoff 的纹素占比，与着色器判据逐位对齐。
    const int n0 = width * height;
    int visible0 = 0;
    for (int i = 0; i < n0; ++i) {
        const float a = static_cast<float>(levels[0].rgba[i * 4 + 3]) / 255.0f;
        if (a >= cutoff) {
            ++visible0;
        }
    }
    const float coverage0 = static_cast<float>(visible0) / static_cast<float>(n0);

    while (levels.back().width > 1 || levels.back().height > 1) {
        const MipLevel& src = levels.back();
        // GL 归约规则：下一级 = max(1, floor(尺寸 / 2))。
        const int dw = (src.width > 1) ? (src.width / 2) : 1;
        const int dh = (src.height > 1) ? (src.height / 2) : 1;
        MipLevel dst;
        dst.width = dw;
        dst.height = dh;
        dst.rgba.resize(static_cast<size_t>(dw) * dh * 4);
        coverage_mipmap_detail::BoxDownsample(src.rgba.data(), src.width, src.height,
                                              dst.rgba.data(), dw, dh);
        coverage_mipmap_detail::BinarizeAlphaToCoverage(dst.rgba.data(), dw, dh,
                                                        coverage0);
        levels.push_back(std::move(dst));
    }
    return levels;
}

}  // namespace jpov

#endif  // JPOV_SRC_COVERAGE_MIPMAP_H_
