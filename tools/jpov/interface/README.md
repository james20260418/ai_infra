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

### InputSnapshot 内存布局

```
mouse: 4×float + 3×int8_t = ~19 bytes
click_pool: 8×3×3×float = ~72 bytes (8 clicks, 3 buttons, 3 fields each)
keys: 348×int8_t = 348 bytes
padding: ~4 bytes
Total: ~440 bytes
```

整个 InputSnapshot 约 **440 bytes**，完全栈分配，无堆分配。

## Proto 转换预留

InputSnapshot 的字段可以直接映射为 protobuf：

```protobuf
message FrameInput {
    // 鼠标
    float mouse_x = 1;
    float mouse_y = 2;
    float mouse_dx = 3;
    float mouse_dy = 4;
    float scroll_delta = 5;
    sint32 mouse_left = 6;    // int8_t 编码
    sint32 mouse_right = 7;
    sint32 mouse_middle = 8;

    // 单击详情（只存有事件的）
    message ClickEvent {
        float x = 1;
        float y = 2;
        float time_ratio = 3;
    }
    repeated ClickEvent clicks = 9;

    // 键盘（只存非 None 的键）
    message KeyEntry {
        uint32 key = 1;
        sint32 state = 2;  // int8_t 编码
    }
    repeated KeyEntry keys = 10;
}
```

proto 序列化时用 `repeated` 只存有数据的条目，比全量 348 个键紧凑得多。
