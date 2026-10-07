# clothing/ — JPOV 穿衣工具的衣物资产库

> 本目录存放 **JPOV 穿衣工具（`tools/jpov/clothing/`）用到的衣物资产**。
> 每件衣服一个子目录，内含：`.glb` 模型 + 生成预览图 +（图像模式下）参考输入图。
> 资产为**外部生成**（gen3d/tripo3d），入库目的是让「穿衣管线」的输入可复现、可引用，
> **不承载 gold test 或渲染回归断言**（生成资产非本项目可控的渲染基准）。

## 目前资产

| 子目录 | 衣服 | 生成方式 | 三角面 | 图元 | 双面(doublesided) |
|---|---|---|---|---|---|
| `harness_vest/` | 战术板载背心（橄榄绿，MOLLE + cummerbund + 弹匣袋） | 单图重建（`--image`） | 9858 | 1 | **是（双面）** |
| `yoga_pants/`  | 女式瑜伽裤（浅灰，宽腰带 10cm + 腰带 "JPOV" 浮雕字） | 文本（`--prompt`） | 9750 | 1 | 否（单面） |

两件刻意选**上衣 / 下装**、**双面 / 单面**两个正交维度：刚好覆盖穿衣工具在这两类
表面朝向下的表现（2026-09-28 Danis 定）。

## 生成溯源（2026-09-28）

工具：`tools/jpov/gen3d_static.sh`（gen3d/static，Tripo P1 低模）。每件 ≈ 3.5 RMB credits。

### harness_vest（战术背心）— 单图模式
```bash
bash tools/jpov/gen3d_static.sh output/gen3d harness_vest \
    --image /james_pm/ai_生图/harness1.png \
    --triangles 10000
```
- 参考输入图：本目录 `harness_vest/input_harness1.png`（608×597 RGBA，alpha 全不透明）。
- 产物 GLB 内嵌贴图：baseColor(2048²) + metallicRoughness(ORM) + normal，各 2048²。

### yoga_pants（瑜伽裤）— 文本模式
```bash
bash tools/jpov/gen3d_static.sh output/gen3d yoga_pants \
    --prompt "维多利亚的秘密风格女式瑜伽裤（紧身打底裤，leggings），浅灰色；高腰，腰带宽度约10厘米，腰带为松紧带材质；腰带正面中央有'JPOV'四个大写英文字母logo；贴身弹力面料，修身剪裁，长裤" \
    --triangles 10000
```
- 贴图 baseColor 均色 RGB ≈ (149,152,158) = 浅灰（渲染图偏深是场景光照 / tone map 所致）。

## 面数预算说明

`--triangles 10000`：蓝人参考体（`mixamo_male.glb`）约 2 万面，故单件上半身/下半身衣物
取 1 万面量级（Tripo 实际落在 9750~9858）。低模下限不能太低（此前实测 500 面会碎）。

## 四视图预览

每件衣服目录下有 `*_{front,up,left,perspective}.png`（`jpov_model_viewer --four_views`，1280×720）。
预览观感为「形状对、局部粗糙」——单图/文本重建的典型局限，非工具链缺陷。

## 在测试/工具里引用

见同级 `test/object3d/BUILD` 的 `clothing_data` filegroup，例如：

```bash
./output/jpov_clothing_tool/jpov_clothing_tool \
    --cloth_path tools/jpov/assets/models/clothing/yoga_pants/yoga_pants.glb
# 人体 reference 缺省 = tools/jpov/assets/models/characters/mixamo_male.glb
```

## 成品样例（成功典范，2026-10-05 Danis 定）

`harness_vest/harness_vest_skinned_sample.glb` —— `harness_vest` 对齐到 `mixamo_male`、
经穿衣工具**自动蒙皮**后保存的标准成品（带 skin / 24 骨）。作为穿衣管线的「正确结果」
参考与回归对照。用法见工具说明 `tools/jpov/clothing/README.md`。

> 区别：`harness_vest.glb` 是**原始生成物**（未对齐、无骨骼）；`..._skinned_sample.glb` 是
> 经穿衣工具处理后的**成品**。二者同为入库资材，但角色不同。
