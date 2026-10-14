// JPOV SMAA（Enhanced Subpixel Morphological Antialiasing）着色器
//
// 本文件是 Jimenez et al. 2011 参考实现（SMAA.hlsl，MIT 许可，见
// tools/jpov/src/smaa/smaa_area_tex.h 顶部版权）到 **GLSL 330 core** 的移植，
// 只保留 **SMAA 1x 空间模式** 的三趟：
//
//   input ─► [SMAALumaEdgeDetectionPS]  ─► edgesTex
//          ─► [SMAABlendingWeightCalculationPS] ─► blendTex
//          ─► [SMAANeighborhoodBlendingPS] ─► output
//
// 未移植：temporal（T2x）/ spatial MS（S2x）/ reprojection / predication /
// stencil 优化。边检测用 **luma**（参考推荐的首选），可选 depth 留待后续。
//
// 与参考实现的关键差异（有意为之）：
//   · 参考在边检测里 `discard`（配合 stencil 跳过无边缘像素）。我们不用 stencil
//     （JPOV 3D 目标已无 stencil），改为无条件写出 edges（无边缘 = 0，与清屏值一致）
//     —— 语义等价且免依赖 stencil。
//   · 搜索步数 / corner rounding 用参考 **HIGH 预设**（编译期常量，见下）；
//     只有 edge threshold 走 uniform（供查看器实时对比）。
//   · 所有纹理采样按参考的 GLSL 路径（linear + clamp），LUT 由 host 侧按
//     RG8 / R8 上传（SMAA_AREATEX_SELECT = .rg，SMAA_SEARCHTEX_SELECT = .r）。
//
// 坐标/分辨率约定：本套 shader 全部在 **输出（viewport）分辨率** 下运行；
// uRtMetrics = vec4(1/w, 1/h, w, h)，随每次 Apply 由 host 传入。

#ifndef JPOV_SRC_SMAA_SMAA_SHADER_H_
#define JPOV_SRC_SMAA_SMAA_SHADER_H_

namespace jpov {

// ── 预置常量（参考 SMAA_PRESET_HIGH）──
// 以 GLSL 字面量硬编码进 shader（GLSL 的循环边界须为常量）。
// 与参考一致：THRESHOLD 0.1（走 uniform）、MAX_SEARCH_STEPS 16、
// MAX_SEARCH_STEPS_DIAG 8、CORNER_ROUNDING 25。
// （THRESHOLD 在参考里是编译期常量；这里改成 uniform 以便查看器实时调参。）

// ── 三趟共用的全屏三角形 VS 片段 ──
// gl_VertexID 覆盖 NDC 的超大三角形（避免对角线过度着色，参考也这么要求）。
// 各趟的 offset 计算不同，故分三个 VS。
static const char* kSmaaEdgeDetectVs = R"glsl(
#version 330 core
uniform vec4 uRtMetrics;   // xy = 1/width, 1/height；zw = width, height
out vec2 vTexCoord;
out vec4 vOffset[3];
void main() {
    vec2 pos;
    if (gl_VertexID == 0) pos = vec2(-1.0, -1.0);
    else if (gl_VertexID == 1) pos = vec2( 3.0, -1.0);
    else pos = vec2(-1.0,  3.0);
    vTexCoord = pos * 0.5 + 0.5;
    vOffset[0] = uRtMetrics.xyxy * vec4(-1.0, 0.0, 0.0, -1.0) + vTexCoord.xyxy;
    vOffset[1] = uRtMetrics.xyxy * vec4( 1.0, 0.0, 0.0,  1.0) + vTexCoord.xyxy;
    vOffset[2] = uRtMetrics.xyxy * vec4(-2.0, 0.0, 0.0, -2.0) + vTexCoord.xyxy;
    gl_Position = vec4(pos, 0.0, 1.0);
}
)glsl";

// ── 第 1 趟：luma 边检测 ──
// 输入应为 **gamma 空间**（sRGB 编码后）的颜色 —— tone map 输出已 sRGB，采样为非 sRGB
// 纹理，符合参考对 luma 边检测的要求。输出 edges.r = 水平边（西侧），edges.g = 垂直边
//（北侧）；无边缘 = 0（不 discard，语义等价于参考的 stencil 优化）。
static const char* kSmaaEdgeDetectFs = R"glsl(
#version 330 core
in vec2 vTexCoord;
in vec4 vOffset[3];
out vec4 FragColor;
uniform sampler2D uColorTex;
uniform float uThreshold;
void main() {
    vec2 threshold = vec2(uThreshold);
    const vec3 weights = vec3(0.2126, 0.7152, 0.0722);
    float L = dot(textureLod(uColorTex, vTexCoord, 0.0).rgb, weights);
    float Lleft = dot(textureLod(uColorTex, vOffset[0].xy, 0.0).rgb, weights);
    float Ltop  = dot(textureLod(uColorTex, vOffset[0].zw, 0.0).rgb, weights);

    vec4 delta;
    delta.xy = abs(L - vec2(Lleft, Ltop));
    vec2 edges = step(threshold, delta.xy);

    float Lright  = dot(textureLod(uColorTex, vOffset[1].xy, 0.0).rgb, weights);
    float Lbottom = dot(textureLod(uColorTex, vOffset[1].zw, 0.0).rgb, weights);
    delta.zw = abs(L - vec2(Lright, Lbottom));

    vec2 maxDelta = max(delta.xy, delta.zw);

    float Lleftleft = dot(textureLod(uColorTex, vOffset[2].xy, 0.0).rgb, weights);
    float Ltoptop   = dot(textureLod(uColorTex, vOffset[2].zw, 0.0).rgb, weights);
    delta.zw = abs(vec2(Lleft, Ltop) - vec2(Lleftleft, Ltoptop));

    maxDelta = max(maxDelta.xy, delta.zw);
    float finalDelta = max(maxDelta.x, maxDelta.y);

    edges *= step(finalDelta, 2.0 * delta.xy);
    FragColor = vec4(edges, 0.0, 1.0);
}
)glsl";

// ── 第 2 趟：混合权重计算 —— VS ──
static const char* kSmaaBlendWeightVs = R"glsl(
#version 330 core
uniform vec4 uRtMetrics;
out vec2 vTexCoord;
out vec2 vPixCoord;
out vec4 vOffset[3];
void main() {
    vec2 pos;
    if (gl_VertexID == 0) pos = vec2(-1.0, -1.0);
    else if (gl_VertexID == 1) pos = vec2( 3.0, -1.0);
    else pos = vec2(-1.0,  3.0);
    vTexCoord = pos * 0.5 + 0.5;
    vPixCoord = vTexCoord * uRtMetrics.zw;
    vOffset[0] = uRtMetrics.xyxy * vec4(-0.25, -0.125, 1.25, -0.125) + vTexCoord.xyxy;
    vOffset[1] = uRtMetrics.xyxy * vec4(-0.125, -0.25, -0.125, 1.25) + vTexCoord.xyxy;
    vOffset[2] = uRtMetrics.xxyy *
                 (vec4(-2.0, 2.0, -2.0, 2.0) * 16.0) +
                 vec4(vOffset[0].xz, vOffset[1].yw);
    gl_Position = vec4(pos, 0.0, 1.0);
}
)glsl";

// ── 第 2 趟：混合权重计算 —— FS ──
// 读 edgesTex，用 areaTex / searchTex 做正交 + 对角线模式搜索，输出每像素的
// 混合权重（r/g = 水平边两侧权重，b/a = 垂直边两侧权重）。SMAA 1x：subsample 全 0。
static const char* kSmaaBlendWeightFs = R"glsl(
#version 330 core
in vec2 vTexCoord;
in vec2 vPixCoord;
in vec4 vOffset[3];
out vec4 FragColor;
uniform sampler2D uEdgesTex;
uniform sampler2D uAreaTex;
uniform sampler2D uSearchTex;
uniform vec4 uRtMetrics;

const int   SMAA_MAX_SEARCH_STEPS      = 16;
const int   SMAA_MAX_SEARCH_STEPS_DIAG = 8;
const float SMAA_AREATEX_MAX_DISTANCE      = 16.0;
const float SMAA_AREATEX_MAX_DISTANCE_DIAG = 20.0;
const vec2  SMAA_AREATEX_PIXEL_SIZE  = vec2(0.00625, 0.00178571428571429); // 1/160, 1/560
const float SMAA_AREATEX_SUBTEX_SIZE = 0.142857142857143;                  // 1/7
const vec2  SMAA_SEARCHTEX_SIZE        = vec2(66.0, 33.0);
const vec2  SMAA_SEARCHTEX_PACKED_SIZE = vec2(64.0, 16.0);
const float SMAA_CORNER_ROUNDING_NORM  = 0.25;   // 25 / 100

void SmaaMovc(bvec2 cond, inout vec2 variable, vec2 value) {
    if (cond.x) variable.x = value.x;
    if (cond.y) variable.y = value.y;
}
void SmaaMovc(bvec4 cond, inout vec4 variable, vec4 value) {
    if (cond.x) variable.x = value.x;
    if (cond.y) variable.y = value.y;
    if (cond.z) variable.z = value.z;
    if (cond.w) variable.w = value.w;
}

// 从一次双线性访问里解码两个对角边缘位（见参考 @DecodeDiagBilinearAccess）。
vec2 SmaaDecodeDiagBilinearAccess(vec2 e) {
    e.r = e.r * abs(5.0 * e.r - 5.0 * 0.75);
    return round(e);
}
vec4 SmaaDecodeDiagBilinearAccess(vec4 e) {
    e.rb = e.rb * abs(5.0 * e.rb - 5.0 * 0.75);
    return round(e);
}

// 对角搜索（两个方向），返回 (距离, 命中端)。
vec2 SmaaSearchDiag1(vec2 texcoord, vec2 dir, out vec2 e) {
    vec4 coord = vec4(texcoord, -1.0, 1.0);
    vec3 t = vec3(uRtMetrics.xy, 1.0);
    while (coord.z < float(SMAA_MAX_SEARCH_STEPS_DIAG - 1) && coord.w > 0.9) {
        coord.xyz = t * vec3(dir, 1.0) + coord.xyz;
        e = textureLod(uEdgesTex, coord.xy, 0.0).rg;
        coord.w = dot(e, vec2(0.5, 0.5));
    }
    return coord.zw;
}
vec2 SmaaSearchDiag2(vec2 texcoord, vec2 dir, out vec2 e) {
    vec4 coord = vec4(texcoord, -1.0, 1.0);
    coord.x += 0.25 * uRtMetrics.x;
    vec3 t = vec3(uRtMetrics.xy, 1.0);
    while (coord.z < float(SMAA_MAX_SEARCH_STEPS_DIAG - 1) && coord.w > 0.9) {
        coord.xyz = t * vec3(dir, 1.0) + coord.xyz;
        e = textureLod(uEdgesTex, coord.xy, 0.0).rg;
        e = SmaaDecodeDiagBilinearAccess(e);
        coord.w = dot(e, vec2(0.5, 0.5));
    }
    return coord.zw;
}

// 对角模式对应的面积查表。
vec2 SmaaAreaDiag(vec2 dist, vec2 e, float offset) {
    vec2 texcoord = vec2(SMAA_AREATEX_MAX_DISTANCE_DIAG, SMAA_AREATEX_MAX_DISTANCE_DIAG) * e + dist;
    texcoord = SMAA_AREATEX_PIXEL_SIZE * texcoord + 0.5 * SMAA_AREATEX_PIXEL_SIZE;
    texcoord.x += 0.5;
    texcoord.y += SMAA_AREATEX_SUBTEX_SIZE * offset;
    return textureLod(uAreaTex, texcoord, 0.0).rg;
}

vec2 SmaaCalculateDiagWeights(vec2 texcoord, vec2 e, vec4 subsampleIndices) {
    vec2 weights = vec2(0.0, 0.0);

    vec4 d;
    vec2 end;
    if (e.r > 0.0) {
        d.xz = SmaaSearchDiag1(texcoord, vec2(-1.0, 1.0), end);
        d.x += float(end.y > 0.9);
    } else {
        d.xz = vec2(0.0, 0.0);
    }
    d.yw = SmaaSearchDiag1(texcoord, vec2(1.0, -1.0), end);

    if (d.x + d.y > 2.0) {
        vec4 coords = vec4(-d.x + 0.25, d.x, d.y, -d.y - 0.25) * uRtMetrics.xyxy + texcoord.xyxy;
        vec4 c;
        c.xy = textureLodOffset(uEdgesTex, coords.xy, 0.0, ivec2(-1, 0)).rg;
        c.zw = textureLodOffset(uEdgesTex, coords.zw, 0.0, ivec2( 1, 0)).rg;
        c.yxwz = SmaaDecodeDiagBilinearAccess(c.xyzw);

        vec2 cc = vec2(2.0) * c.xz + c.yw;
        SmaaMovc(bvec2(step(0.9, d.zw)), cc, vec2(0.0, 0.0));
        weights += SmaaAreaDiag(d.xy, cc, subsampleIndices.z);
    }

    d.xz = SmaaSearchDiag2(texcoord, vec2(-1.0, -1.0), end);
    if (textureLodOffset(uEdgesTex, texcoord, 0.0, ivec2(1, 0)).r > 0.0) {
        d.yw = SmaaSearchDiag2(texcoord, vec2(1.0, 1.0), end);
        d.y += float(end.y > 0.9);
    } else {
        d.yw = vec2(0.0, 0.0);
    }

    if (d.x + d.y > 2.0) {
        vec4 coords = vec4(-d.x, -d.x, d.y, d.y) * uRtMetrics.xyxy + texcoord.xyxy;
        vec4 c;
        c.x  = textureLodOffset(uEdgesTex, coords.xy, 0.0, ivec2(-1, 0)).g;
        c.y  = textureLodOffset(uEdgesTex, coords.xy, 0.0, ivec2( 0, -1)).r;
        c.zw = textureLodOffset(uEdgesTex, coords.zw, 0.0, ivec2( 1, 0)).gr;
        vec2 cc = vec2(2.0) * c.xz + c.yw;
        SmaaMovc(bvec2(step(0.9, d.zw)), cc, vec2(0.0, 0.0));
        weights += SmaaAreaDiag(d.xy, cc, subsampleIndices.w).gr;
    }

    return weights;
}

// 搜索纹理查表（加 0/1/2 长度修正）。
float SmaaSearchLength(vec2 e, float offset) {
    vec2 scale = SMAA_SEARCHTEX_SIZE * vec2(0.5, -1.0);
    vec2 bias  = SMAA_SEARCHTEX_SIZE * vec2(offset, 1.0);
    scale += vec2(-1.0, 1.0);
    bias  += vec2(0.5, -0.5);
    scale *= 1.0 / SMAA_SEARCHTEX_PACKED_SIZE;
    bias  *= 1.0 / SMAA_SEARCHTEX_PACKED_SIZE;
    return textureLod(uSearchTex, scale * e + bias, 0.0).r;
}

float SmaaSearchXLeft(vec2 texcoord, float end) {
    vec2 e = vec2(0.0, 1.0);
    while (texcoord.x > end && e.g > 0.8281 && e.r == 0.0) {
        e = textureLod(uEdgesTex, texcoord, 0.0).rg;
        texcoord = -vec2(2.0, 0.0) * uRtMetrics.xy + texcoord;
    }
    float offset = -(255.0 / 127.0) * SmaaSearchLength(e, 0.0) + 3.25;
    return uRtMetrics.x * offset + texcoord.x;
}
float SmaaSearchXRight(vec2 texcoord, float end) {
    vec2 e = vec2(0.0, 1.0);
    while (texcoord.x < end && e.g > 0.8281 && e.r == 0.0) {
        e = textureLod(uEdgesTex, texcoord, 0.0).rg;
        texcoord = vec2(2.0, 0.0) * uRtMetrics.xy + texcoord;
    }
    float offset = -(255.0 / 127.0) * SmaaSearchLength(e, 0.5) + 3.25;
    return -uRtMetrics.x * offset + texcoord.x;
}
float SmaaSearchYUp(vec2 texcoord, float end) {
    vec2 e = vec2(1.0, 0.0);
    while (texcoord.y > end && e.r > 0.8281 && e.g == 0.0) {
        e = textureLod(uEdgesTex, texcoord, 0.0).rg;
        texcoord = -vec2(0.0, 2.0) * uRtMetrics.xy + texcoord;
    }
    float offset = -(255.0 / 127.0) * SmaaSearchLength(e.gr, 0.0) + 3.25;
    return uRtMetrics.y * offset + texcoord.y;
}
float SmaaSearchYDown(vec2 texcoord, float end) {
    vec2 e = vec2(1.0, 0.0);
    while (texcoord.y < end && e.r > 0.8281 && e.g == 0.0) {
        e = textureLod(uEdgesTex, texcoord, 0.0).rg;
        texcoord = vec2(0.0, 2.0) * uRtMetrics.xy + texcoord;
    }
    float offset = -(255.0 / 127.0) * SmaaSearchLength(e.gr, 0.5) + 3.25;
    return -uRtMetrics.y * offset + texcoord.y;
}

vec2 SmaaArea(vec2 dist, float e1, float e2, float offset) {
    vec2 texcoord = vec2(SMAA_AREATEX_MAX_DISTANCE, SMAA_AREATEX_MAX_DISTANCE) *
                    round(4.0 * vec2(e1, e2)) + dist;
    texcoord = SMAA_AREATEX_PIXEL_SIZE * texcoord + 0.5 * SMAA_AREATEX_PIXEL_SIZE;
    texcoord.y = SMAA_AREATEX_SUBTEX_SIZE * offset + texcoord.y;
    return textureLod(uAreaTex, texcoord, 0.0).rg;
}

void SmaaDetectHorizontalCornerPattern(inout vec2 weights, vec4 texcoord, vec2 d) {
    vec2 leftRight = step(d.xy, d.yx);
    vec2 rounding = (1.0 - SMAA_CORNER_ROUNDING_NORM) * leftRight;
    rounding /= leftRight.x + leftRight.y;
    vec2 factor = vec2(1.0, 1.0);
    factor.x -= rounding.x * textureLodOffset(uEdgesTex, texcoord.xy, 0.0, ivec2(0, 1)).r;
    factor.x -= rounding.y * textureLodOffset(uEdgesTex, texcoord.zw, 0.0, ivec2(1, 1)).r;
    factor.y -= rounding.x * textureLodOffset(uEdgesTex, texcoord.xy, 0.0, ivec2(0, -2)).r;
    factor.y -= rounding.y * textureLodOffset(uEdgesTex, texcoord.zw, 0.0, ivec2(1, -2)).r;
    weights *= clamp(factor, 0.0, 1.0);
}
void SmaaDetectVerticalCornerPattern(inout vec2 weights, vec4 texcoord, vec2 d) {
    vec2 leftRight = step(d.xy, d.yx);
    vec2 rounding = (1.0 - SMAA_CORNER_ROUNDING_NORM) * leftRight;
    rounding /= leftRight.x + leftRight.y;
    vec2 factor = vec2(1.0, 1.0);
    factor.x -= rounding.x * textureLodOffset(uEdgesTex, texcoord.xy, 0.0, ivec2( 1, 0)).g;
    factor.x -= rounding.y * textureLodOffset(uEdgesTex, texcoord.zw, 0.0, ivec2( 1, 1)).g;
    factor.y -= rounding.x * textureLodOffset(uEdgesTex, texcoord.xy, 0.0, ivec2(-2, 0)).g;
    factor.y -= rounding.y * textureLodOffset(uEdgesTex, texcoord.zw, 0.0, ivec2(-2, 1)).g;
    weights *= clamp(factor, 0.0, 1.0);
}

void main() {
    const vec4 subsampleIndices = vec4(0.0);   // SMAA 1x
    vec4 weights = vec4(0.0, 0.0, 0.0, 0.0);
    vec2 e = texture(uEdgesTex, vTexCoord).rg;

    if (e.g > 0.0) {   // 北侧有边
        weights.rg = SmaaCalculateDiagWeights(vTexCoord, e, subsampleIndices);
        // 对角线优先：命中对角线则跳过正交处理。
        if (weights.r == -weights.g) {
            vec2 d;
            vec3 coords;
            coords.x = SmaaSearchXLeft(vOffset[0].xy, vOffset[2].x);
            coords.y = vOffset[1].y;
            d.x = coords.x;
            float e1 = textureLod(uEdgesTex, coords.xy, 0.0).r;
            coords.z = SmaaSearchXRight(vOffset[0].zw, vOffset[2].y);
            d.y = coords.z;
            d = abs(round(uRtMetrics.zz * d - vPixCoord.xx));
            vec2 sqrt_d = sqrt(d);
            float e2 = textureLodOffset(uEdgesTex, coords.zy, 0.0, ivec2(1, 0)).r;
            vec2 w_hv = SmaaArea(sqrt_d, e1, e2, subsampleIndices.y);
            coords.y = vTexCoord.y;
            SmaaDetectHorizontalCornerPattern(w_hv, coords.xyzy, d);
            weights.rg = w_hv;
        } else {
            e.r = 0.0;   // 跳过垂直处理
        }
    }

    if (e.r > 0.0) {   // 西侧有边
        vec2 d;
        vec3 coords;
        coords.y = SmaaSearchYUp(vOffset[1].xy, vOffset[2].z);
        coords.x = vOffset[0].x;
        d.x = coords.y;
        float e1 = textureLod(uEdgesTex, coords.xy, 0.0).g;
        coords.z = SmaaSearchYDown(vOffset[1].zw, vOffset[2].w);
        d.y = coords.z;
        d = abs(round(uRtMetrics.ww * d - vPixCoord.yy));
        vec2 sqrt_d = sqrt(d);
        float e2 = textureLodOffset(uEdgesTex, coords.xz, 0.0, ivec2(0, 1)).g;
        vec2 w_bv = SmaaArea(sqrt_d, e1, e2, subsampleIndices.x);
        coords.x = vTexCoord.x;
        SmaaDetectVerticalCornerPattern(w_bv, coords.xyxz, d);
        weights.ba = w_bv;
    }

    FragColor = weights;
}
)glsl";

// ── 第 3 趟：邻域混合 —— VS ──
static const char* kSmaaNeighborBlendVs = R"glsl(
#version 330 core
uniform vec4 uRtMetrics;
out vec2 vTexCoord;
out vec4 vOffset;
void main() {
    vec2 pos;
    if (gl_VertexID == 0) pos = vec2(-1.0, -1.0);
    else if (gl_VertexID == 1) pos = vec2( 3.0, -1.0);
    else pos = vec2(-1.0,  3.0);
    vTexCoord = pos * 0.5 + 0.5;
    vOffset = uRtMetrics.xyxy * vec4(1.0, 0.0, 0.0, 1.0) + vTexCoord.xyxy;
    gl_Position = vec4(pos, 0.0, 1.0);
}
)glsl";

// ── 第 3 趟：邻域混合 —— FS ──
// 读 blendTex 的权重，按水平/垂直方向在两侧邻居间做（利用双线性的）加权混合。
static const char* kSmaaNeighborBlendFs = R"glsl(
#version 330 core
in vec2 vTexCoord;
in vec4 vOffset;
out vec4 FragColor;
uniform sampler2D uColorTex;
uniform sampler2D uBlendTex;
uniform vec4 uRtMetrics;

void main() {
    vec4 a;
    a.x = texture(uBlendTex, vOffset.xy).a;   // 右
    a.y = texture(uBlendTex, vOffset.zw).g;   // 上
    vec4 bt = texture(uBlendTex, vTexCoord);
    a.w = bt.x;                               // 左
    a.z = bt.z;                               // 下

    if (dot(a, vec4(1.0, 1.0, 1.0, 1.0)) < 1e-5) {
        FragColor = textureLod(uColorTex, vTexCoord, 0.0);
    } else {
        bool h = max(a.x, a.z) > max(a.y, a.w);
        vec4 blendingOffset = vec4(0.0, a.y, 0.0, a.w);
        vec2 blendingWeight = a.yw;
        if (h) {
            blendingOffset = vec4(a.x, 0.0, a.z, 0.0);
            blendingWeight = a.xz;
        }
        blendingWeight /= dot(blendingWeight, vec2(1.0, 1.0));
        vec4 blendingCoord = blendingOffset * vec4(uRtMetrics.xy, -uRtMetrics.xy) + vTexCoord.xyxy;
        vec4 color = blendingWeight.x * textureLod(uColorTex, blendingCoord.xy, 0.0);
        color += blendingWeight.y * textureLod(uColorTex, blendingCoord.zw, 0.0);
        FragColor = color;
    }
}
)glsl";

}  // namespace jpov

#endif  // JPOV_SRC_SMAA_SMAA_SHADER_H_
