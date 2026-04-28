# JPOV Interface

JPOV 的纯头文件接口定义，每个接口对应一个独立的 cc_library，方便按需引用。

## 文件与库

| cc_library | 文件 | 说明 |
|------------|------|------|
| `input_snapshot` | `input_snapshot.h` | 帧级输入抽象 InputSnapshot |
| `camera` | `camera.h` | 透视相机配置 Camera（含浮点视口矩形） |
| `window_info` | `window_info.h` | 帧级窗口信息 WindowInfo（宽/高/宽高比） |
| `render_command` | `render_command.h` | 渲染指令输出 RenderCommandList（含各类图元命令） |
| `jpov_interface` | 全部 | 聚合所有接口，方便一次性引用 |

## 编码方案

### 鼠标/键盘状态编码（int8_t）

| 编码值 | MouseState | KeyState |
|--------|-----------|----------|
| -1 | Drag | Hold |
| 0 | None | None |
| 1~8 | Click(n) | Click(n) |

Click 的数值 = 本帧单击次数。上限 8（1fps × 250ms CLICK_DELTA ≈ 4 次，给余量）。

### 坐标系统

- **2D 屏幕坐标**：原点在窗口左上角，x 向右为正，y 向下为正。float 类型，支持子像素精度。
- **3D 世界坐标**：右手系（x 向右，y 向上，z 向后）。

### 依赖关系

```
jpov_interface
├── camera        → geom/common:vec
├── input_snapshot                  → glog_static
├── render_command → geom/common:vec, glog_static
└── window_info                      → glog_static
```

### InputSnapshot 内存布局

```
mouse: 5×float                    = 20 bytes
mouse state: 3×int8_t + 1 padding =  4 bytes
click events: 3×8×3×float         = 72 bytes  (3 buttons, 8 clicks, 3 fields)
keys: 348×int8_t                  = 348 bytes
Total: ~444 bytes
```

整个 InputSnapshot 约 **444 bytes**，完全栈分配，无堆分配。

## 编码规范

### CHECK 保护规则

凡是有 crash 风险的函数（如数组索引访问），必须：
1. 在头文件声明时加 `// Pre-condition:` 注释说明输入参数约束
2. 内部用 `CHECK_*` 宏快速失败

不允许"静默处理"越界输入——crash 是暴露 bug 的途径。

## Proto 转换预留

InputSnapshot 的字段可以直接映射为 protobuf：

```protobuf
message FrameInput {
    float mouse_x = 1;
    float mouse_y = 2;
    float mouse_dx = 3;
    float mouse_dy = 4;
    float scroll_delta = 5;
    sint32 mouse_left = 6;    // int8_t 编码
    sint32 mouse_right = 7;
    sint32 mouse_middle = 8;

    message ClickEvent {
        float x = 1;
        float y = 2;
        float time_ratio = 3;
    }
    repeated ClickEvent clicks = 9;

    message KeyEntry {
        uint32 key = 1;
        sint32 state = 2;  // int8_t 编码
    }
    repeated KeyEntry keys = 10;
}
```

proto 序列化时用 `repeated` 只存有数据的条目，比全量 348 个键紧凑得多。
