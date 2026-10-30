#ifndef JPOV_JPOV_H_
#define JPOV_JPOV_H_

#include <cstdint>
#include <memory>

#ifndef _WIN32
#include <csignal>
#include <sys/types.h>
#endif

#include <GLFW/glfw3.h>

#include "tools/jpov/interface/input_snapshot.h"
#include "tools/jpov/interface/debug_info.h"
#include "tools/jpov/interface/gltf_object.h"
#include "tools/jpov/src/gltf_loader.h"
#include "tools/jpov/interface/mesh.h"
#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/interface/window_info.h"
#include "tools/jpov/src/renderer.h"

// ============================================================================
// JPOV — 轻型渲染窗口框架
//
// 生命周期：
//   1. Init(config)  — 创建 GL context + Renderer
//   2. Run()         — 事件循环（多帧）
//    或
//      RunOnce()    — 单帧 → 截图输出
//   3. Finalize()    — 销毁所有资源
//
//  Run() 内部每帧调用 RunOnceInternal()，与 RunOnce() 共享相同的渲染核心，
//  确保在线与离线输出一致。
//
// 用法:
//   class MyApp : public JPOV {
//       void OneIteration(int64_t frame_count,
//                         const InputSnapshot& input,
//                         const WindowInfo& winfo,
//                         RenderCommandList* cmds) override {
//           // 画东西...
//       }
//   };
//
//   int main() {
//       MyApp app(cfg);
//       app.Init();
//       app.Run();
//       app.Finalize();
//   }
// ============================================================================
class JPOV {
public:
    struct FontEntry {
        const char* path;         // TTF/TTC 文件路径，UTF-8
        int         ttc_index   = 0;  // TTC font index（单 TTF 忽略）
        const char* alias;            // 字体别名，DrawText 通过它引用
        // 最多允许注册 kMaxFonts 种字体
    };

    static constexpr int kMaxFonts = 10;

    struct Config {
        const char* title        = "JPOV";
        int         width        = 1280;
        int         height       = 720;
        bool        resizable    = true;
        bool        fullscreen   = false;
        bool        show_console = false;  // Windows 下是否显示命令行窗口

        // font_entries: 用户注册的字体列表（最多 kMaxFonts 项）
        // 每个 FontEntry 包含字体文件路径、TTC index、用户指定的别名。
        // 加载失败或别名重复时 Init() 会 LOG(FATAL) crash。
        // JPOV 不提供隐式默认字体：请在此显式声明每个字体（path/ttc_index/alias）。
        std::vector<FontEntry> fonts;

        // headless = true: 无可见窗口（适用于 CI / 后台截图）
        // headless = false: 正常窗口（Run() 进入事件循环）
        bool        headless     = false;

        // OneIteration 调用帧率（Hz），默认 60。
        // Run() 按此帧率调度 OneIteration，实际帧率受 vsync 限制。
        // 设为 0 表示不限制（尽可能快）。
        int target_fps = 60;

        // 全局阴影配置（CSM）。默认 = ShadowConfig::Default()（开放世界通用）。
        // 无 sun 时该配置不生效；有 sun 时按此切分级联、每段渲染独立 shadow map。
        jpov::ShadowConfig shadow;

        // 左上角 debug HUD（硬件信息 / 相机机位 / 分辨率 / 帧号 / fps）的**初始可见性**。
        //   true（默认）：**常驻**。这是引擎的既有约定之一 —— 任何 app 一跑起来就能
        //                 看到自己跑在什么 GPU 上、多少 fps；不想看的人按 F3 收起，
        //                 或者在自己 app 的 Config 里显式设 false。
        //   false：启动时关闭（运行中按 F3 仍可打开）。
        // ⚠️ 与 headless 的关系（**唯一一处例外，写在这里不是静默行为**）：
        //    headless=true 时初值恒为 false。headless 的产物是截图，而截图会被
        //    逐像素对比，叠 9 行 HUD 会让所有既有 gold 变成非确定。headless 下
        //    想看 HUD 请显式调 SetDebugOverlayVisible(true)。
        // `F3` 是**引擎保留键**：切换它，且应用在 InputSnapshot 里看不到这次按键。
        // 选 F3 而不是 Quake 系的 `~`：`~` 会把 grave accent 从 InputSnapshot 里
        // 吞掉，文本框（ui.cc / sp_ui.cc）就再也输不了 '`' / '~'；功能键没有这个
        // 代价，也符合「类似 Minecraft 的独立游戏」(docs/jpov_engine_integration.md:3)
        // 的惯例（F3 = debug 面板）。
        // HUD 用内置 8x8 ASCII 点阵字体绘制（纯白、左上角），**不依赖** fonts 列表
        // —— 零字体注册的 app 也能显示。宽度契约见 jpov::DebugInfo::kMaxLineChars
        // （单行 <= 500px，与窗口宽度无关；窗口更窄时右侧被裁，不会崩也不会缩字）。
        bool        debug_overlay = true;

        // debug HUD 的字形放大倍数（整数倍字节点阵缩放，1 = 8x8 原生像素）。
        // 1 是「还能看清」的下限（点阵字体再小就得换 5x7 字模，中文更不可能），
        // 同时也是能塞下最多信息的档位。
        //
        // 默认值配合 debug_overlay_scale_follow_dpi：屏缩放 100% ⇒ 1，200% ⇒ 2。
        // 这个字段只在 follow_dpi = false 时生效（作为显式覆盖值）。
        // Pre-condition: 1 <= debug_overlay_scale <= 8（越界 Init() 会 CHECK 失败）
        int         debug_overlay_scale = 1;

        // 是否让 debug HUD 的字号跟随窗口的 DPI 缩放（glfwGetWindowContentScale）。
        //   true（默认）：100% 屏 = 8 物理像素（现状），200% 屏 = 16 物理像素，
        //                 也就是「看起来」一直是 8 逻辑像素 —— 视觉大小不随 DPI 变。
        //   false：严格用 debug_overlay_scale 的整数值（老行为）。
        // 为什么需要它：8 物理像素在 200% 缩放的屏上只有 4 个逻辑像素，会看不清；
        // 而「固定物理像素」在混合 DPI 环境下永远照顾不到两头。
        bool        debug_overlay_scale_follow_dpi = true;
    };

    explicit JPOV(Config cfg);
    virtual ~JPOV();

    // ---- 生命周期 ----

    // Init: 创建 GL context + Renderer。
    // headless=false 时创建可见窗口并注册 GLFW 回调。
    // headless=true  时创建隐藏窗口（仅用于 GL context）。
    // Pre-condition: 未调用过 Init() 或已调用 Finalize()
    void Init();

    // Finalize: 销毁 Renderer + GL context + 窗口。
    // 调用后可以再次 Init()。
    void Finalize();

    // ---- 运行模式 ----

    // Run: 窗口事件循环（阻塞）。
    // 每帧：PollEvents → CaptureInput → RunOnceInternal → Present → SwapBuffers。
    // Pre-condition: Init() 已调用且 window_ 非空
    void Run();

    // RunOnce: 单帧执行 + 截图输出
    //
    // 模拟一帧的完整流程：OneIteration(frame_count) → RunOnceInternal →
    // SaveScreenshot。输出图片为窗口尺寸（win_w × win_h）。
    //
    // frame_count  — input: 本帧的**帧号**，原样传给 OneIteration。多帧仿真时由
    //                调用方递增（0,1,2,…）⇒ OneIteration 可以据此自计时。
    // input       — input: 模拟的输入快照（可完全伪造，见 input_snapshot.h 的编码）
    // winfo       — input: 模拟的窗口信息（宽度/高度决定截图尺寸）
    // out_png_path — output: 截图保存路径
    //
    // Pre-condition: Init() 已调用
    // Pre-condition: frame_count >= 0
    // Pre-condition: winfo.width > 0 && winfo.height > 0
    // Pre-condition: out_png_path 非空
    void RunOnce(int64_t frame_count, const jpov::InputSnapshot& input,
                 const jpov::WindowInfo& winfo,
                 const char* out_png_path);

    // 便利重载：等价于 RunOnce(0, …)（单帧快照场景）。
    // 需要「多帧仿真/自计时」时用带 frame_count 的版本。
    void RunOnce(const jpov::InputSnapshot& input,
                 const jpov::WindowInfo& winfo,
                 const char* out_png_path);

    // 测量文本以指定字号的绘制宽度（像素），语义与 DrawText2D 的布局推进
    // 完全一致（pen 水平终点 = 光标 X）。委托 Renderer/FontRenderer，用真实
    // 字体进宽定位——适用于 UI 输入框光标等需要与渲染层一致宽度的场景。
    //   alias: 字体别名；空串 → 首个注册字体；未知别名 → crash。
    // Pre-condition: Init() 已调用；font_size > 0
    float MeasureTextWidth(const std::string& alias, const std::string& text,
                           float font_size);

    // ---- 用户需实现的纯虚函数 ----

    // 用户实现的每帧渲染逻辑
    //
    // frame_count — input: 从 0 开始的帧计数器，单调递增
    // input       — input: 本帧输入快照（鼠标/键盘状态）
    // winfo       — input: 本帧窗口尺寸信息
    // cmds        — output: 填充渲染指令列表，帧末由框架消费。非空指针。
    //
    // Pre-condition: frame_count >= 0
    // Pre-condition: cmds != nullptr
    virtual void OneIteration(int64_t frame_count,
                              const jpov::InputSnapshot& input,
                              const jpov::WindowInfo& winfo,
                              jpov::RenderCommandList* cmds /*output*/) = 0;

    // ---- 纹理管理 ----

    // RegisterTexture: 从 PNG 文件加载纹理到 GPU。
    //
    // 返回纹理 ID 用于 DrawImage。
    // 同一文件路径仅加载一次，重复调用返回相同 ID。
    // 加载失败 → LOG(FATAL) crash。
    //
    // Pre-condition: Init() 已调用
    // Pre-condition: filepath 非空
    uint32_t RegisterTexture(const std::string& filepath);

    // RegisterTexture: 带采样选项加载 PNG 纹理到 GPU。
    //
    // opts.mipmap: true → 生成 mipmap（三线性），大透视平铺面防摩尔纹。
    // opts.repeat: true → GL_REPEAT（UV>1 周期重复平铺）。
    // 默认皆 false（等价无选项版 RegisterTexture）。
    // 同一路径以不同选项加载 → 各自独立纹理（去重 key 含选项位）。
    //
    // Pre-condition: Init() 已调用
    // Pre-condition: filepath 非空
    uint32_t RegisterTexture(const std::string& filepath,
                             const jpov::TextureOptions& opts);

    // RegisterTexture: 注册已有 GL 纹理对象。
    //
    // 调用者自行管理 GL 纹理生命周期。
    // JPOV 仅在 ReleaseTexture 时移除记录，不 delete 外部 GL 纹理。
    //
    // Pre-condition: Init() 已调用
    // Pre-condition: gl_texture_id != 0, width > 0, height > 0
    uint32_t RegisterTexture(uint32_t gl_texture_id, int width, int height);

    // RegisterTexture: 从**内存里的编码图片字节**（PNG/JPEG…）加载纹理到 GPU。
    //
    // 用于「本来就已在内存里」的图（如程序生成 / 编辑后的贴图、glb 内嵌图），无需先落盘。
    // key：去重身份（同一 key + 选项只加载一次、只上传一次）；调用方应给**内容相关**的键，
    //   （如内容哈希），不要用可能与别的图重名的东西。
    //
    // Pre-condition: Init() 已调用
    // Pre-condition: encoded 非空；key 非空
    uint32_t RegisterTextureFromMemory(const std::vector<unsigned char>& encoded,
                                       const std::string& key,
                                       const jpov::TextureOptions& opts = {});

    // ReleaseTexture: 释放纹理。
    //
    // 纹理 ID 不存在 → 静默忽略。
    // Pre-condition: Init() 已调用
    void ReleaseTexture(uint32_t texture_id);

    // RegisterVolumeTexture: 从 .jpovvol 文件（JPVOL2）加载**体积纹理**到 GPU。
    //
    // 返回体积 id，供 cmds->DrawVolumeFire(VolumeFireCommand{volume_id=...}) 引用。
    // 同一路径仅加载一次。加载失败 → 返回 0。
    //
    // Pre-condition: Init() 已调用
    // Pre-condition: filepath 非空
    uint32_t RegisterVolumeTexture(const std::string& filepath);

    // 查询体积纹理的帧数（不存在 → 0）。
    int VolumeFrameCount(uint32_t volume_id) const;

    // 调试：开启体积火 raymarch 中间结果导出（accum=加权色 L、alpha=覆盖 → PNG）。
    // 空串 = 关。每帧一对 `<dir>/dump_NNN_accum.png` / `_alpha.png`。
    void SetVolumeDebugDump(const std::string& dir);

    // ---- 网格管理 ----

    // RegisterMesh: 将 CPU MeshData 上传为 GPU mesh，返回 mesh_id。
    //
    // 用户自行构造 MeshData（positions/normals/uvs/...），本方法负责
    // 校验数据、创建 VAO + 按属性分离的 VBO + EBO，返回可被 DrawObject3D
    // 引用的 mesh_id。
    // 数据不合规（数组长度不一致 / flags 与数据不符）→ LOG(FATAL) crash。
    //
    // Pre-condition: Init() 已调用
    // Pre-condition: data.Validate() 通过（数组对齐、flags 与数据一致）
    uint32_t RegisterMesh(const jpov::MeshData& data);

    // RegisterSkeleton: 将一种骨架（SkeletonType + 一整包 pose）注册为可蒙皮渲染的
    // 骨架资源（SkeletonManager），返回 skeleton_id（非 0）。
    //
    // type  : 骨架定义（joints 树 + rest_offset + bind_rotation，见 LoadGltfSkeleton /
    //         Mixamo23Skeleton 产出）。inverse_bind 是派生量，不再作为字段（需时 ComputeInverseBind）。
    // poses : 该骨架的全部静态位姿关键帧（烘焙成 pose atlas）。静态退化门传
    //         SkeletonPose::Identity(bone_count)（全恒等 = 该骨架的 bind/T-pose）。
    // thickness_scaling_config: 骨架级「部位粗细」全局配置（可选）：至多
    //         kNumThicknessGroup(=8) 个**关节组**，每组一串**关节 index**（= type.joints 下标）；
    //         同组关节共用一个 per-instance 缩放系数，由 SkinnedInstanceState::thickness_scales
    //         按**组号**给出（用来调该部位胖瘦，例：腿一组 / 手臂一组 / Hips 一组）。
    //         index 合法性在注册时立即校验（越界、一骨两组 → LOG(FATAL)）；空组 = 不用；
    //         全空 = 该骨架不做粗细（渲染侧整段跳过，旧场景零回归）。
    //         默认 {} 即"不做粗细"，不是隐藏的魔术默认（"没有配置"就表达"不做"）。
    // partial_rotation_config: 骨架级「部位额外旋转」全局配置（可选）：至多
    //         kNumPartialRotation(=2) 个**关节名**，每个定义一条旋转通道 —— 该关节及其**整个
    //         子树**会被 SkinnedInstanceState::partial_rotations 里同下标的四元数 R
    //         **在模型系**绕该关节的**当前 pose 位置**整体旋转（先摆 pose、再叠加 R；用途：扭腰 / 仰头等）。
    //         “R 相对什么、定义在哪个系”的完整语义（含 TPose 歪头边界）见 interface/skeleton_types.h
    //         的 partial_rotations 字段注释。骨名合法性在注册时立即校验（空 = 该通道不用；
    //         查不到 / 两通道同指一骨 → LOG(FATAL)）；全空 = 不做额外旋转（渲染侧整段跳过，
    //         旧场景零回归）。默认 {} 即"不做"。
    // 返回 skeleton_id 供 cmds->DrawMeshWithSkeleton(mesh_id, skeleton_id, instances) 引用。
    //
    // Pre-condition: Init() 已调用；type.Validate() 通过；poses 非空。
    uint32_t RegisterSkeleton(
        const jpov::SkeletonType& type, std::vector<jpov::SkeletonPose> poses,
        std::array<std::vector<int>, jpov::kNumThicknessGroup> thickness_scaling_config = {},
        std::array<std::string, jpov::kNumPartialRotation> partial_rotation_config = {});

    // UpdateMesh: 更新已有 mesh 的顶点数据。
    //
    // new_data.flags 必须与注册时一致（VBO 布局不能变）。
    // 实现策略：删除旧 GL 资源 → 按新数据重建。
    //
    // Pre-condition: Init() 已调用
    // Pre-condition: mesh_id 已注册
    void UpdateMesh(uint32_t mesh_id, const jpov::MeshData& new_data);

    // ReleaseMesh: 释放 mesh 的 GL 资源。
    //
    // mesh_id 不存在 → 静默忽略。
    // Pre-condition: Init() 已调用
    void ReleaseMesh(uint32_t mesh_id);

    // ReleaseSkeleton: 释放 RegisterSkeleton 返回的骨架资源（回收 pose atlas 等 GL 资源；
    // 槽位回收，后续 RegisterSkeleton 复用）。
    //
    // skeleton_id 为 0 / 不存在 → 静默忽略；释放后该 id 不可再用于 DrawMeshWithSkeleton
    // （否则 LOG(FATAL)）。
    // Pre-condition: Init() 已调用
    void ReleaseSkeleton(uint32_t skeleton_id);

    // ---- glTF 模型加载 ----

    // LoadGltf: 从 .gltf/.glb 文件加载整个模型并上传 GPU 资源。
    //
    // 内部走纯净 loader（无 GL），再用本框架的 MeshManager / TextureManager
    // 上传几何与贴图（自动去重 + ORM 拆包）。返回的 GltfObject 资源独占。
    //
    // 渲染: 在 OneIteration 里用 cmds->DrawGltfObject(obj, ...) 绘制。
    // 释放: 调 ReleaseGltf(obj)。
    //
    // Pre-condition: Init() 已调用
    // 失败返回空 GltfObject（empty()）。
    // options.unit_meters：源文件长度单位 → 米（<=0 = 按文件自述 / 规范默认米）。
    jpov::GltfObject LoadGltf(const std::string& path,
                              const jpov::GltfLoadOptions& options = {});

    // ReleaseGltf: 释放一个 GltfObject 占用的全部 GPU 资源。
    //
    // Pre-condition: Init() 已调用
    void ReleaseGltf(const jpov::GltfObject& gltf);

    // 上一次拾取查询的结果（方法甲：渲染时填入，供下帧 OneIteration 读取）。
    //
    // 用户在 OneIteration 里发起拾取查询（设 cmds->pick.enabled=true + 屏幕坐标），
    // 本帧 Render() 内部跑 color-ID pass 并填好结果；下一帧 OneIteration 调本方法
    // 读到命中内容（hit + picking_id）。因查询结果要等渲染完成才能得出，故有
    // 一帧延迟——需求方按下帧读即可。
    //
    // 未发起过 pick 查询时返回初始状态（hit=false, picking_id=0）。
    const jpov::PickResult& last_pick() const { return renderer_->last_pick(); }

    // ---- Debug 信息 ----

    // 引擎持有的 debug 状态快照（硬件信息 / 本帧相机 / 分辨率 / 帧号 / fps）。
    //
    // 更新时机：Init() 填 gpu（此后不变）；每帧 RunOnceInternal 填其余字段。
    // 用途：app 想在 UI 里显示“当前用哪块 GPU”时直接读它，无需自己写
    // glGetString（也无需自己解析软件光栅器名字）。
    const jpov::DebugInfo& debug() const { return debug_; }

    // 左上角 debug HUD 实际使用的字形放大倍数（Init 时定：跟随 DPI 或取
    // Config::debug_overlay_scale）。headless 的 Xvfb / 普通 100% 屏上通常 = 1。
    int debug_overlay_effective_scale() const { return debug_overlay_scale_; }

    // 左上角 debug HUD 当前是否可见（F3 切换；初值 = Config::debug_overlay）。
    bool is_debug_overlay_visible() const { return debug_overlay_visible_; }

    // 程序设置 debug HUD 可见性（与按 F3 等价；Config::debug_overlay 只是初值）。
    // 典型用法：CI 逐像素对比前 SetDebugOverlayVisible(false)，让截图里没有 HUD。
    // 用途：想把 HUD 写进截图/录像的自动化、或者给 app 自己的“设置”项接一个开关。
    void SetDebugOverlayVisible(bool visible) { debug_overlay_visible_ = visible; }

private:
    // ---- 核心渲染步骤（Run 和 RunOnce 共享） ----
    //
    // 1. 调用 OneIteration → cmds
    // 2. BeginFrame(render_resolution)
    // 3. Render(cmds)  → 绘制到 FBO
    //
    // Pre-condition: renderer_ 已初始化
    void RunOnceInternal(int64_t frame_count,
                         const jpov::InputSnapshot& input,
                         const jpov::WindowInfo& winfo);

    // ---- GLFW 输入状态 ----
    struct MouseButtonState {
        bool is_down = false;
        bool released_this_frame = false;
        double press_time = 0.0;
        bool moved_since_press = false;
    };
    struct KeyButtonState {
        bool is_down = false;
        bool released_this_frame = false;
        int  click_count = 0;
    };
    struct FrameEvents {
        int left_clicks   = 0;
        int right_clicks  = 0;
        int middle_clicks = 0;
        jpov::ClickEvent left_clicks_detail[jpov::kMaxClicksPerFrame];
        jpov::ClickEvent right_clicks_detail[jpov::kMaxClicksPerFrame];
        jpov::ClickEvent middle_clicks_detail[jpov::kMaxClicksPerFrame];
    };

    double mouse_x_       = 0.0;
    double mouse_y_       = 0.0;
    double mouse_last_x_  = 0.0;
    double mouse_last_y_  = 0.0;
    double scroll_delta_  = 0.0;

    MouseButtonState left_btn_;
    MouseButtonState right_btn_;
    MouseButtonState middle_btn_;
    KeyButtonState keys_[jpov::kMaxKeyCode];
    jpov::KeyMods  mods_;   // 本帧修饰键状态（HandleKey 从事件 mods 刷新）
    FrameEvents frame_;
    double frame_start_time_ = 0.0;
    Config config_;
    GLFWwindow* window_ = nullptr;
    std::unique_ptr<jpov::Renderer> renderer_;

    // ---- Debug HUD 状态 ----
    // DebugInfo 是**引擎自己的一份状态**（不是渲染指令）：Init 填 gpu，每帧填相机/fps。
    jpov::DebugInfo debug_;
    // debug HUD 实际生效的字形放大倍数。Init() 里定一次（DPI 在这之后不会变：
    // GLFW 不做运行中 DPI 迁移，换屏也不会回调）。绘制用它，不直接读 Config。
    int debug_overlay_scale_ = 1;

    // 是否显示左上角 debug HUD。初值来自 Config::debug_overlay（默认 true），
    // 运行时由 F3 切换。
    bool debug_overlay_visible_ = false;
    // 上一帧的墙钟时刻（glfwGetTime），用来算瞬时 fps；0 = 还没有上一帧。
    double last_frame_time_ = 0.0;
    pid_t xvfb_pid_    = 0;
    bool  started_xvfb_ = false;
    bool  initialized_  = false;

    // ---- GLFW 回调（静态转发） ----
    static void OnMouseButton(GLFWwindow* window, int button, int action, int mods);
    static void OnMouseMove(GLFWwindow* window, double xpos, double ypos);
    static void OnScroll(GLFWwindow* window, double xoffset, double yoffset);
    static void OnKey(GLFWwindow* window, int key, int scancode, int action, int mods);

    void HandleMouseButton(int button, int action, double now);
    void HandleMouseMove(double xpos, double ypos);
    void HandleScroll(double yoffset);
    void HandleKey(int key, int scancode, int action, int mods);

    void CaptureInput(jpov::InputSnapshot* input /*output*/);
    void RenderCommands(const jpov::RenderCommandList& cmds /*input*/);
    double FrameInterval() const;

    static void FlushMouseButton(const MouseButtonState& btn /*input*/,
                                 int click_count /*input*/,
                                 const jpov::ClickEvent* click_detail /*input*/,
                                 jpov::MouseState* out /*output*/,
                                 jpov::ClickEvent* out_clicks /*output*/);
    void FlushKeyboard(jpov::InputSnapshot* input /*output*/);

    static constexpr double kClickDelta = 0.3;
};

#endif  // JPOV_JPOV_H_
