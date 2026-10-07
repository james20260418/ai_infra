# wallet_tripo_multiview — **正面样例**：Tripo 多视图重建硬表面单体可用

> ✅ **本目录是「images（多视图）模式」的参考样例**：输入 4 张正交视图 →
> 输出可用的立体 PBR 模型。留它是为了给后人一个**有据可查的成功样本**，
> 并作为「什么算合格输入」的对照基准。

## 一句话结论

**4 张真正交视图 → Tripo `multiview-to-model`，对「硬表面单体（皮质手包 +
五金 + 挂件）」可用**：结构完整、无破损空洞、贴图三件套齐全，能直接进
`model_editor` 继续摆位/编辑。
（对照：单图重建复杂植物是反面教材，见 sibling `oak_tripo_negative/`。）

## 实验记录（2026-09-18）

输入（4 张，顺序 `[front, left, back, right]`，均为 2278×1280 JPEG / RGB）：

| 文件 | 视图 |
|---|---|
| `tripo_wallet_input_front.jpg` | 正面 |
| `tripo_wallet_input_left.jpg` | 左侧 |
| `tripo_wallet_input_back.jpg` | 背面 |
| `tripo_wallet_input_right.jpg` | 右侧 |

链路：
```bash
gen3d_cmd generate --name wallet --output_dir <dir> \
    --images front.jpg left.jpg back.jpg right.jpg
# → 上传 4 张 → POST /generation/multiview-to-model → 轮询 → 下载 GLB
```

产物：`tripo_wallet.glb`（**817 KB**，P1-20260311 档，4000 面预算），
单 mesh + PBR 三贴图（baseColor / metallicRoughness / normal），glTF 2.0 合法。

四视图渲染（`jpov_model_viewer --four_views`）：
`tripo_wallet_front.png` / `_left.png` / `_up.png` / `_perspective.png`。

## 验收结论

| 项 | 结果 |
|---|---|
| 立体性 | ✅ 完整立体（非平面贴片）——透视 / 顶视可看出体积 |
| 几何完整性 | ✅ 无破损、无空洞、无穿模 |
| 包体 | ✅ 长方形手包、圆角、**拉链齿与拉链头独立建模** |
| 链条 | ⚠️ 有建模简化：细密环扣与局部椭圆环造型前后不完全一致 |
| 毛绒挂件 | ⚠️ 形状对，**无毛发质感**（像硅胶/橡胶） |
| 贴图 | ⚠️ 偏平涂：皮革无凹凸纹理、金属无明显高光 |
| 水印 | ✅ 输入图右下角有「豆包AI生成」水印，**未被烘成文字浮雕**（预期风险未发生） |

即：**「结构正确、细节粗糙」**——对游戏内中远景道具已可用；要近景写实
仍需后续精修（高模档 / 重拓扑 / 手工修）。

## 关键观察（方法论）

1. **输入质量决定上限。** 这 4 张图本身并不理想：视角**略斜、有透视变形**、
   底部**带阴影**、主体是「包 + 细链条 + 毛绒挂件」这类高频细碎复合体。
   模型仍给出了「结构正确」的结果——这是 Tripo 的极限，不是工具链的问题。
2. **水印没那么可怕**：白底输入图上的角落水印没有被烘成几何浮雕。
   但**仍建议裁掉**，减少不确定性。
3. **白底 > 透明底**：当前链路对白底有官方背书（"clean background"）。
   本次实测白底输入未生成出「基座/底盘」（阴影未实体化）。
4. **面数预算 4000 对硬表面单体够用**（对比植物：20000 面仍救不了）。

## 复现方式

```bash
# 需 TRIPO_API_KEY（会烧 credit；multiview 本次约 20~30 credits）
cd /james_pm/ai_infra
bazel build //tools/jpov/gen3d/static:gen3d_cmd //tools/jpov:jpov_model_viewer

./bazel-bin/tools/jpov/gen3d/static/gen3d_cmd generate \
    --name wallet --output_dir /tmp/wallet \
    --images <本目录>/tripo_wallet_input_front.jpg \
             <本目录>/tripo_wallet_input_left.jpg \
             <本目录>/tripo_wallet_input_back.jpg \
             <本目录>/tripo_wallet_input_right.jpg

DISPLAY=:99 ./bazel-bin/tools/jpov/jpov_model_viewer \
    --four_views --out_dir /tmp/wallet /tmp/wallet/wallet.glb
```

> ⚠️ **若任务中途客户端超时**（本样例实测发生过：服务端已 success，但客户端
> 在 180s 轮询窗口到点后退出、task_id 一度丢失，产物没下载）：
> **不要重跑**（重跑 = 白烧 credit）。用错误信息里的 task_id 续取：
> ```bash
> gen3d_cmd generate --name wallet --output_dir /tmp/wallet \
>     --resume_task <task_id>
> ```

## 用途

- **正向参照**：「什么算合格的 multiview 输入 / 输出」。
- **对照实验**：与 `oak_tripo_negative/` 并看，可看出**输入结构复杂度**
  如何决定成败——硬表面单体 vs 高自遮挡密叶植物。
- 如需在测试里引用，见同目录 `BUILD` 的 `wallet_tripo_multiview_data` filegroup。
  **注意：它只为「留证 / 参照」存在，不承载 gold test 或渲染回归断言**——
  这是外部生成资产，不是本项目可控的渲染基准。
