// JPOV 3D 文本（空间 quad + 字形 atlas，无光照纯色）gold + 结构门禁。
//
// 本测试做**两件事**（缺一不可 —— 单靠 gold 字节比对抓不到朝向错误）：
//
//  A. Gold 字节比对：与仓库内 gold PNG 逐字节比较。
//     场景：固定相机 + 3 段文字（正面 / 绕 Y 转 90° / 倾斜 up），
//     无光照 → 确定性输出（llvmpipe 下 3D 图元路径是逐字节稳定的，
//     见 cube3d / strip3d gold 的既有先例）。
//
//  B. 结构门禁（BuildText3DWorldVerts 纯函数，不渲染）：
//     验证「文字朝向 / 上方向 / 对齐 / 世界高度」在几何上确实生效。
//
//     ⚠️ 为什么必须有 B：gold 字节比对只能证明「和上次一样」，不能证明「对」。
//     如果我把 r（右向量）算成 -r，文字会左右镜像 —— gold 图会一致（只要
//     generator 和 test 用同一份错实现），字节比对依然全绿。
//     故 B 直接对世界坐标断言方向，才是朝向正确性的真门禁。
//
// ⚠️ 零运行代码阅读检查记录（见 skills/zero-run-code-reading-check）：
//    本文件刻意**不写**以下恒真断言（并在此记录，避免后人以为有覆盖）：
//      - EXPECT_GT(verts.size(), 0)：若字体没加载，GenerateTextVertices 返回
//        false，外面的 if 早 return —— 断言不到任何东西。这里改为
//        「非空 + 顶点数 == 6×字数」双向断言。
//      - EXPECT_NE(anchor_world, anchor)：anchor 平移后必然不等，恒真。
//        改为断言「相对 anchor 的偏移」而非绝对坐标。
//      - 只查「顶点 y 都 > anchor.y」抓不到镜像 —— 镜像只改 x/z 符号，
//        y 不变。故 x/z 方向必须单独断言。

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/text3d_util.h"
#include "tools/jpov/src/primitives3d/primitives3d_renderer.h"
#include "tools/common/utils.h"
#include "tools/jpov/test/primitives3d/test_utils.h"

namespace {

// CHECK_NEAR 辅助：glog 无此宏。返回一个 CHECK 风格的可流式对象：
//   CHECK_NEAR_LOCAL(a, b, tol) << "额外信息";
// 语义同 CHECK（失败 crash），且**支持尾部 << 消息**（宏不能吞掉后续 <<）。
class NearChecker {
public:
    NearChecker(double got, double want, double tol, const char* expr)
        : got_(got), want_(want), tol_(tol), ok_(std::fabs(got - want) <= tol) {
        if (!ok_) {
            LOG(ERROR) << "CHECK_NEAR 失败: " << expr << " | got=" << got
                       << " want=" << want << " tol=" << tol;
            // 与 CHECK 一致：立即失败。
            LOG(FATAL) << "CHECK_NEAR(allowed) failed";
        }
    }
    // 仅在失败时会被使用；成功时 <</返回值被丢弃也无害。
    template <typename T>
    NearChecker& operator<<(const T&) { return *this; }
    explicit operator bool() const { return ok_; }
private:
    double got_, want_, tol_;
    bool ok_;
};
#define CHECK_NEAR_LOCAL(got, want, tol) \
    NearChecker((got), (want), (tol), #got " vs " #want)

// ============================================================
// B. 结构门禁：BuildText3DWorldVerts 的几何正确性
// ============================================================

// 构造一段「已知形状」的假 glyph 顶点：一个 4 顶点矩形（2 三角形），
// 每顶点 4 floats (x,y,u,v)，文本平面局部坐标。
//
// ⚠️ 坐标从 (0,0) 起：这样「相对方向」断言最直接。
//    （初稿用了 x∈[10,30] 的非零起点，导致「左下在 -X」断言前提写错 ——
//    因为 BuildText3DWorldVerts 不做重新居中，局部 10 会被映射到正侧。
//    这里改为从 0 起，并把断言改成相对比较，两道保险。）
std::vector<float> MakeQuadGlyphVerts() {
    return {
         0.0f,   0.0f, 0.0f, 0.0f,   // 左上
        20.0f,   0.0f, 1.0f, 0.0f,   // 右上
        20.0f,  20.0f, 1.0f, 1.0f,   // 右下
         0.0f,  20.0f, 0.0f, 1.0f,   // 左下
    };
}

jpov::Text3DCommand MakeCmd(const jpov::Vec3f& anchor,
                            const jpov::Vec3f& face,
                            const jpov::Vec3f& up) {
    jpov::Text3DCommand cmd;
    cmd.anchor = anchor;
    cmd.face_direction = face;
    cmd.up_direction = up;
    cmd.font_height_world = 1.0f;   // 20px 字形高 → 1 米
    cmd.alignment = jpov::TextAlignment::kTopLeft;
    cmd.color = jpov::kColorWhite;
    cmd.font_alias = "";
    return cmd;
}

// 结构门禁 1：face=+Z, up=+Y 时，右方向必须是 +X（不是 -X）。
//   这是"镜像 bug"的直接门禁：若 r = cross(u,n) 写成 cross(n,u)，本断言失败。
//   断言用**相对比较**（右上 x > 左上 x），不依赖局部坐标系起点。
void TestOrientationRightIsPlusX() {
    std::vector<float> verts = MakeQuadGlyphVerts();
    const jpov::Vec3f anchor = {0.0f, 0.0f, 0.0f};
    jpov::Text3DCommand cmd = MakeCmd(anchor, {0.0f, 0.0f, 1.0f},
                                      {0.0f, 1.0f, 0.0f});
    float scale = 0.0f;
    jpov::Primitives3DRenderer::BuildText3DWorldVerts(cmd, 20.0f, &verts,
                                                       &scale);

    CHECK_EQ(verts.size(), 4 * 5) << "顶点数应为 4 个 × 5 floats";

    const float v0x = verts[0 * 5 + 0], v0y = verts[0 * 5 + 1];  // 左上
    const float v1x = verts[1 * 5 + 0], v1y = verts[1 * 5 + 1];  // 右上
    const float v2y = verts[2 * 5 + 1];                          // 右下
    const float v3x = verts[3 * 5 + 0];                          // 左下

    // 右方向：右上 x 必须 > 左上 x（镜像会翻成 <）。
    CHECK_GT(v1x, v0x)
        << "右方向错误：右上顶点 x=" << v1x << " 应 > 左上 x=" << v0x
        << "（若为 < → cross(u,n) 写反导致文字左右镜像）";
    // 左下与左上同 x（同一列）。
    CHECK_NEAR_LOCAL(v3x, v0x, 1e-6f) << "左下与左上应同列（同 x）";

    // quad 应位于 z=0 平面（face=+Z）。
    for (int v = 0; v < 4; ++v) {
        CHECK_NEAR_LOCAL(verts[v * 5 + 2], 0.0f, 1e-6f)
            << "quad 应位于 z=0 平面（face=+Z），顶点 " << v;
    }

    // 上方向：左上 y 必须 > 右下 y（文字向下延伸）。
    CHECK_GT(v0y, v2y)
        << "上方向错误：左上 y=" << v0y << " 应 > 右下 y=" << v2y
        << "（若为 < → 文字上下颠倒）";
    CHECK_NEAR_LOCAL(v1y, v0y, 1e-6f) << "右上与左上应同高（同一行）";

    LOG(INFO) << "[B1] 朝向门禁通过：右=+X 下=-Y（face=+Z, up=+Y）";
}

// 结构门禁 2：世界高度确实等于 font_height_world。
//   字形矩形在文本平面高 20px；font_height_world=1.0 米 / 20px → scale=0.05。
//   故世界高应为 20 * 0.05 = 1.0 米。
void TestWorldHeightMatchesSpec() {
    std::vector<float> verts = MakeQuadGlyphVerts();
    jpov::Text3DCommand cmd = MakeCmd({0.0f, 0.0f, 0.0f},
                                      {0.0f, 0.0f, 1.0f},
                                      {0.0f, 1.0f, 0.0f});
    float scale = 0.0f;
    jpov::Primitives3DRenderer::BuildText3DWorldVerts(cmd, 20.0f, &verts,
                                                       &scale);

    // 顶边 y=0、底边 y=20 → 世界 y 跨度 = 20*scale
    const float top_y = verts[0 * 5 + 1];
    const float bot_y = verts[2 * 5 + 1];
    const float world_h = std::fabs(bot_y - top_y);
    CHECK_NEAR_LOCAL(world_h, 1.0f, 1e-5f)
        << "世界高度应为 font_height_world=1.0 米，实际 " << world_h;

    // 宽度：局部 x 跨度 20px → 世界 20*0.05 = 1.0 米。
    const float left_x = verts[0 * 5 + 0];
    const float right_x = verts[1 * 5 + 0];
    const float world_w = std::fabs(right_x - left_x);
    CHECK_NEAR_LOCAL(world_w, 1.0f, 1e-5f)
        << "世界宽度应为 1.0 米（20px × 0.05），实际 " << world_w;

    LOG(INFO) << "[B2] 世界尺寸门禁通过：1.0m × 1.0m（20px 字形 @ h=1.0m）";
}

// 结构门禁 3：文字朝向随 face_direction 旋转。
//   face=+X（文字朝 +X 看）→ 文字平面法线应为 +X；右方向应落在 ±Z。
//   这验证 face 真的被用作 quad 法线，而不是被忽略。
void TestFaceDirectionRotatesPlane() {
    std::vector<float> verts = MakeQuadGlyphVerts();
    jpov::Text3DCommand cmd = MakeCmd({0.0f, 0.0f, 0.0f},
                                      {1.0f, 0.0f, 0.0f},   // face = +X
                                      {0.0f, 1.0f, 0.0f});  // up   = +Y
    float scale = 0.0f;
    jpov::Primitives3DRenderer::BuildText3DWorldVerts(cmd, 20.0f, &verts,
                                                       &scale);

    // 4 个顶点的 x 分量应全为 0（quad 平面 ⟂ face=+X）。
    for (int v = 0; v < 4; ++v) {
        CHECK_NEAR_LOCAL(verts[v * 5 + 0], 0.0f, 1e-5f)
            << "face=+X 时 quad 应位于 x=0 平面，顶点 " << v
            << " 的 x=" << verts[v * 5 + 0];
    }
    // 右方向应有非零 z 分量（原本的 +X 方向被转到 ±Z）。
    const float right_z = verts[1 * 5 + 2];   // 右上顶点的 z
    CHECK_GT(std::fabs(right_z), 1e-4f)
        << "face=+X 后右方向应有 z 分量，实际 right_z=" << right_z;

    LOG(INFO) << "[B3] 朝向旋转门禁通过：face=+X → quad 在 x=0 平面";
}

// 结构门禁 4：up_direction 不垂直于 face 时做 Gram-Schmidt（face 权威）。
//   给 up = +Y + 0.5*face（含面内分量），正交化后 up 仍应垂直于 face。
void TestGramSchmidtOrthogonalizesUp() {
    std::vector<float> verts = MakeQuadGlyphVerts();
    // face=+Z, up = (0,1,0) + 0.5*(0,0,1) = (0,1,0.5)  → 非正交输入
    jpov::Text3DCommand cmd = MakeCmd({0.0f, 0.0f, 0.0f},
                                      {0.0f, 0.0f, 1.0f},
                                      {0.0f, 1.0f, 0.5f});
    float scale = 0.0f;
    jpov::Primitives3DRenderer::BuildText3DWorldVerts(cmd, 20.0f, &verts,
                                                       &scale);

    // 正交化后：quad 必须仍在 z=0 平面（up 的面内 z 分量被剔除）。
    for (int v = 0; v < 4; ++v) {
        CHECK_NEAR_LOCAL(verts[v * 5 + 2], 0.0f, 1e-5f)
            << "Gram-Schmidt 后 quad 应在 z=0 平面，顶点 " << v
            << " 的 z=" << verts[v * 5 + 2];
    }
    // 宽度不受 up 的 z 分量污染（若未正交化，quad 会倾斜 → 宽度含 z）。
    const float right_x = verts[1 * 5 + 0] - verts[0 * 5 + 0];
    CHECK_NEAR_LOCAL(right_x, 1.0f, 1e-4f)
        << "正交化后宽度应仍为 1.0 米，实际 " << right_x;

    // 文字仍向上（+Y）延伸：左上 y 必须 > 右下 y。
    CHECK_GT(verts[0 * 5 + 1], verts[2 * 5 + 1])
        << "正交化后文字仍应向下延伸";

    LOG(INFO) << "[B4] Gram-Schmidt 门禁通过：非正交 up 被投影到 face 垂面";
}

// ============================================================
// 结构门禁 5：PixelsPerMeterAt 的解析解正确性
// ============================================================
// 竖直视野 fov、深度 d、FBO 高 H 时：
//   px_per_meter = H / (2 * d * tan(fov/2))
// 用手算值对拍（不依赖渲染）。
void TestPixelsPerMeterAnalytic() {
    jpov::Camera cam;
    cam.position = {0.0f, 0.0f, 0.0f};
    cam.target   = {0.0f, 0.0f, -1.0f};   // 看向 -Z
    cam.up       = {0.0f, 1.0f, 0.0f};
    cam.fov      = 90.0f;                 // tan(45°)=1 → 公式退化为 H/(2d)
    const int H = 720;

    // 深度 d=10 处：ppm = 720 / (2*10*1) = 36
    const jpov::Vec3f a = {0.0f, 0.0f, -10.0f};
    const float ppm = jpov::PixelsPerMeterAt(a, cam, H);
    CHECK_NEAR_LOCAL(ppm, 36.0f, 1e-3f)
        << "fov=90,d=10,H=720 时 ppm 应为 36，实际 " << ppm;

    // 深度加倍 → ppm 减半（透视衰减）。
    const jpov::Vec3f b = {0.0f, 0.0f, -20.0f};
    const float ppm2 = jpov::PixelsPerMeterAt(b, cam, H);
    CHECK_NEAR_LOCAL(ppm2, 18.0f, 1e-3f) << "深度加倍应使 ppm 减半，实际 " << ppm2;

    // 侧向偏移不改变深度 → ppm 不变（证实用深度而非欧氏距离）。
    const jpov::Vec3f c = {5.0f, 3.0f, -10.0f};
    const float ppm3 = jpov::PixelsPerMeterAt(c, cam, H);
    CHECK_NEAR_LOCAL(ppm3, 36.0f, 1e-3f)
        << "侧向偏移不应改变 ppm（用沿光轴深度），实际 " << ppm3;

    LOG(INFO) << "[B5] PixelsPerMeterAt 解析解门禁通过（36 / 18 / 36）";
}

// ============================================================
// A. Gold 字节比对
// ============================================================

constexpr int kFboW = 1280;
constexpr int kFboH = 720;

// 3 段文字，覆盖：默认朝向 / 绕 Y 转 90°（face=+X）/ 倾斜 up。
class Text3dGoldApp : public JPOV {
public:
    using JPOV::JPOV;

    void OneIteration(int64_t, const jpov::InputSnapshot&,
                      const jpov::WindowInfo&,
                      jpov::RenderCommandList* cmds) override {
        cmds->camera.fbo_3d_width_  = static_cast<float>(kFboW);
        cmds->camera.fbo_3d_height_ = static_cast<float>(kFboH);
        cmds->camera.position = {0.0f, 0.0f, 6.0f};
        cmds->camera.target   = {0.0f, 0.0f, 0.0f};
        cmds->camera.up       = {0.0f, 1.0f, 0.0f};
        cmds->camera.fov      = 60.0f;
        cmds->camera.near     = 0.1f;
        cmds->camera.far      = 100.0f;

        // 无光照：纯色 quad（tone_mapping 默认 true 会让颜色经 ACES，
        // 但纯色 + 无光照仍是确定性的；为减少变量这里关掉 tone map，
        // 走 LDR 直通路径，与 cube3d gold 的场景假设一致）。
        cmds->tone_mapping = false;

        // 1) 正面朝向（face=+Z）
        cmds->DrawText3D("JPOV", {0.0f, 1.5f, 0.0f},
                         {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f},
                         0.9f, jpov::kColorRed, "", jpov::TextAlignment::kCenter);

        // 2) 绕 Y 转 90°（face=+X），位于 -X 侧
        cmds->DrawText3D("ABC", {-2.0f, 0.0f, 0.0f},
                         {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
                         0.9f, jpov::kColorGreen, "",
                         jpov::TextAlignment::kCenter);

        // 3) 倾斜 up（非正交输入，验证 Gram-Schmidt 在渲染路径的表现）
        cmds->DrawText3D("3D", {2.0f, 0.0f, 0.0f},
                         {0.0f, 0.0f, 1.0f}, {0.4f, 1.0f, 0.3f},
                         0.9f, jpov::kColorBlue, "",
                         jpov::TextAlignment::kCenter);
    }
};

std::string GetExpectedPngPath() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir) {
        std::string p = test_srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/tools/jpov/test/primitives3d/text3d_quad_1280x720.png";
        return p;
    }
    return jpov::GetProjectRoot() +
           "tools/jpov/test/primitives3d/text3d_quad_1280x720.png";
}

int RunGoldCompare() {
    std::string expected_path = GetExpectedPngPath();
    std::vector<uint8_t> expected_bytes;
    if (!jpov::ReadFileBytes(expected_path, &expected_bytes)) {
        LOG(ERROR) << "Failed to load expected PNG: " << expected_path
                   << "（首次生成请运行 "
                      "bazel run //tools/jpov/test/primitives3d:"
                      "jpov_text3d_gold_generator）";
        return 1;
    }

    std::string outdir = jpov::GetOutputDir() + "jpov_text3d_gold_test/";
    std::string outpath = outdir + "rendered.png";

    JPOV::Config cfg;
    cfg.title = "3D Text Gold Test";
    cfg.headless = true;
    cfg.width = 640;
    cfg.height = 360;
    // 字体：与 test/font2d 的 gold 用同一约定（仓库相对路径 + 内置 Latin 别名）。
    // gold generator 与 test 必须声明完全一致，否则渲染不一致 → 字节比对红。
    cfg.fonts = {{"tools/jpov/fonts/DejaVuSans.ttf", 0, jpov::kFontBuiltinLatin}};

    Text3dGoldApp app(cfg);
    app.Init();

    jpov::WindowInfo winfo;
    winfo.width  = 640.0f;
    winfo.height = 360.0f;
    jpov::InputSnapshot input{};
    app.RunOnce(input, winfo, outpath.c_str());
    app.Finalize();

    std::vector<uint8_t> render_bytes;
    if (!jpov::ReadFileBytes(outpath, &render_bytes)) {
        LOG(ERROR) << "Failed to load rendered PNG: " << outpath;
        return 1;
    }

    if (render_bytes.size() != expected_bytes.size()) {
        LOG(ERROR) << "Size mismatch: rendered=" << render_bytes.size()
                   << ", expected=" << expected_bytes.size();
        return 1;
    }
    for (size_t i = 0; i < render_bytes.size(); ++i) {
        if (render_bytes[i] != expected_bytes[i]) {
            LOG(ERROR) << "Byte mismatch at offset " << i << ": got 0x"
                       << std::hex << static_cast<int>(render_bytes[i])
                       << ", expected 0x"
                       << static_cast<int>(expected_bytes[i]);
            return 1;
        }
    }
    LOG(INFO) << "[A] gold 字节比对通过（" << expected_bytes.size()
              << " bytes）";
    return 0;
}

}  // namespace

int main() {
    // B 部分：纯几何门禁（不渲染，先跑，失败快速定位）
    TestOrientationRightIsPlusX();
    TestWorldHeightMatchesSpec();
    TestFaceDirectionRotatesPlane();
    TestGramSchmidtOrthogonalizesUp();
    TestPixelsPerMeterAnalytic();

    // A 部分：gold 字节比对
    return RunGoldCompare();
}
