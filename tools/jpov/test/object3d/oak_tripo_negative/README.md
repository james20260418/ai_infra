# oak_tripo_negative — **反面教材**：Tripo 单图重建复杂植物不可用

> ⚠️ **本目录不是「可用资产」，是「不要这么做」的证据。**
> 保留它是为了给后人（包括未来的 agent）一个**有据可查的失败样本**，
> 避免重复踩坑、重复烧 credit。

## 一句话结论

**单张照片 → Tripo 3D 重建，对「结构复杂的植物（密叶树冠 + 发达根系）」
不可用。游戏里这类资产应老实用「纸片 + 透明纹理（billboard / alpha card）」
做，不要指望 AI 单图重建。**

## 实验记录（2026-09-18）

输入：`tripo_oak_input.png`（2048×2048，一棵写实橡树照片/渲染图，纯白背景）

> 注：原始文件名为 `oak.jpg`，但**实际内容是 PNG**（magic bytes 与扩展名不符）。
> 我们的 `image_input.cc` 按内容判定，正确识别——见
> `gen3d/static/image_input.h` 的 magic bytes 检查。

链路：`gen3d_cmd generate --image ...` → `image-to-model` → GLB → `jpov_model_viewer --four_views`

| 产物 | 模型 | 面数 | 结果 |
|---|---|---|---|
| `tripo_oak_4k.glb` | P1-20260311 | **3,859** | ❌ 树冠碎成飘浮「碎纸片」 |
| `tripo_oak_20k.glb` | P1-20260311 | **19,436** | ⚠️ 枝干救回来了，**树冠本质未变**（仍是离散面片堆叠、透空率高） |

两个 GLB 均：单 mesh、PBR 三贴图齐全（baseColor / metallicRoughness / normal）、
glTF 2.0 合法、JPOV loader 能正常加载渲染。**模型文件本身没有损坏——
是「重建出来的形状」不对。**

## 关键观察

1. **拉满面数只解决了「枝干」，没解决「树冠」。**
   4000 面 → 4000 面里的树干/枝是粗糙的；20000 面下枝干变得连续、有粗细变化。
   但树冠在两者下都是「离散多边形面片堆叠」，未形成连续叶面。

2. **根因不是面数，是「单图重建的固有限制」。**
   照片里的树叶是**互相遮挡的一团**，AI 没有足够信息推断「每片叶子在哪、朝向如何」，
   只能猜。猜的结果就是一堆朝向随机的块状物。这是信息量问题，不是预算问题。

3. **因此：换 H 系列高模（量变）大概率也救不了。** 不要为此再烧 credit。

## 顺带得到的两个方法论结论（已写进设计文档）

### ① 低模必须「先高模再减面」，不能直接低模出模

直接在低面数预算下出模，AI 是在**「压缩」（被迫丢结构）**而不是
**「简化」（保形状减面）**——所以出来的**形状本身就是错的**，再简单也不会对。

正路：
```
高模出模（形状正确）
  → POST /v3/mesh/decimate 重拓扑（保形状，只减面）
  → 低模
```
Tripo 有现成的重拓扑端点（Retopology 10~30 credits）。**尚未实现**，
列为后续工作。

### ② 植物类资产的正确做法

**纸片（billboard / alpha card）+ 透明纹理**，这是业界标准做法
（也是 JPOV 静态渲染明确支持的路：`text3d_depth_alpha_*` 那套已验证
带 alpha 的贴片渲染可用）。

## 复现方式

```bash
# 需 TRIPO_API_KEY（会烧 credit；本实验两次共约 20 credits）
cd /james_pm/ai_infra
bazel build //tools/jpov/gen3d/static:gen3d_cmd //tools/jpov:jpov_model_viewer

./bazel-bin/tools/jpov/gen3d/static/gen3d_cmd generate \
    --name oak --output_dir /tmp/oak --image <本目录>/tripo_oak_input.png \
    --triangles 20000

DISPLAY=:99 ./bazel-bin/tools/jpov/jpov_model_viewer \
    --four_views --output_dir /tmp/oak /tmp/oak/oak.glb
```

## 用途

- **反向参照**：当你觉得「AI 生成的东西看着还行」时，拿这个看看什么叫「不行」。
- **决策依据**：「要不要用 Tripo 生成某类资产」——植物这类高自遮挡、薄片密集的
  结构，答案是**不要**。
- 如需在测试里引用，见同目录 `BUILD` 的 `oak_tripo_negative_data` filegroup。
  **注意：它只为「留证」存在，不承载任何 gold test / 渲染回归断言**——
  拿一个已知畸形的模型做 gold 基准是没有意义的。
