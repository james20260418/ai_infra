# JPOV Interface

JPOV 的纯头文件接口定义，可被 OneIteration 用户直接 include。

## 文件

| 文件 | 说明 |
|------|------|
| `input_snapshot.h` | 帧级输入抽象 InputSnapshot |

## 编码方案

### 鼠标/键盘状态编码（int8_t）

| 编码值 | MouseState | KeyState |
|--------|-----------|----------|
| -1 | Drag | Hold |
| 0 | None | None |
| 1~8 | Click(n) | Click(n) |

Click 的数值 = 本帧单击次数。上限 8（1fps × 250ms CLICK_DELTA ≈ 4 次，给余量）。

### 坐标系统

屏幕坐标：原点在窗口左上角，x 向右为正，y 向下为正。值类型为 float，支持子像素精度。

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
2. 内部用 `__builtin_trap()` （或后续用 LOG(FATAL)）快速失败

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
