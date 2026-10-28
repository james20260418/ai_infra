# configs/ —— 入库的「生成配置」

这里放的是**输入**：一棵树该怎么长（喂给 Blender/Sapling 的那组参数）。
产物（`shape.vegshape`）是**输出**，不落在这个目录，也不入 git。

- schema + 每个字段的含义/单位/效果：**`../proto/generate.proto`**（要看参数先看它）
- 生成：`../gen/shape_gen`（C++，推荐）；`../generate_shape.py` 留作对拍基准

| 文件 | 树 | 预设 | 规模 | 生成耗时* |
|---|---|---|---|---|
| `pine.pbtxt` | 松树（塔形、裸干、轮生枝） | `small_pine` | 331 枝 / 5.4k 卡 | ~2 s |
| `oak.pbtxt` | 橡树（宽冠阔叶、粗干） | `small_maple` + 球冠 | 703 枝 / 21k 卡 | ~9 s |
| `fir.pbtxt` | 杉树（塔形、下垂枝、四级分枝） | `douglas_fir` | 4100 枝 / 12.6k 卡 | ~2 min |

\* 本机实测（Linux 容器、单线程）—— **Python 版**的耗时；C++ 版 `gen/shape_gen` 是 0.6 / 1.3 / 1.5 s（同配置同产物）。
  耗时**按枝数平方**增长，原因见下面「已知问题」。

```bash
# 从仓库根：配置 -> shape.vegshape（Blender 只负责倒数据）
./bazel-bin/tools/jpov/tools/vegetation_tool_v2/gen/shape_gen \
    --config tools/jpov/tools/vegetation_tool_v2/configs/pine.pbtxt \
    --out output/veg_shapes/pine.vegshape

# 只用扩展名决定产物格式（`.vegshape` = 二进制，其余 = 文本），
# 所以出资产就写 .vegshape；只想看文本时加 --text 或直接写 .pbtxt。
# （注意：`--dry-run` 只在 Python 版的 generate_shape.py 里有，shape_gen 不认这个参数。）

# 用产物出图（在已构建的查看器目录里）
cd output/jpov_vegetation_v2_viewer
./jpov_vegetation_v2_viewer --shape ../veg_shapes/pine.vegshape \
    --bark bark/bark001 --leaf leaf/willow_leaf.png --leaf2 leaf/willow_leaf_flip.png \
    --leaf-black 30 --capture shots/pine
```

## 新增一个配置

1. 复制最接近的一份（乔木用 `pine`/`oak`，针叶用 `fir`），改 `name` 与需要的字段；
2. **没写的字段 = 用预设自带值**（不是 0）。只写你真的想改的，注释里写清「为什么是这个数」；
3. 产物如果拓扑序不合法，工具会直接报错（见下）；只想看参数就先 `--dry-run` 跑
   Python 版 `generate_shape.py`（C++ 的 `gen/shape_gen` 没有这个开关）。

## 已知问题（会挡住某些配置）

- **爬山虎没进这个目录**：Sapling 没有藤本/攀援生成器，出不来「沿墙蔓延」；当时的近似做法是
  「generate 之后把 Z 压扁 + 卡面统一朝墙」，那属于后处理，不是生成配置能表达的东西。
- **灌木也没进来**：`baseSplits=7` 那类基部分裂会触发导出脚本的拓扑序缺陷（见下），
  这一轮先不纳入；要加得先修那个缺陷。
- **导出脚本的已知缺陷**（会让产物被 `Validate` 拒收，工具会在生成后检查并**报错退出**）：
  BFS 拓扑重排的「兜底」把不可达 spline 直接追加到末尾 → 子枝可能排在父枝之后。
  触发条件跟 `baseSplits` / 枝数有关（灌水那类基部分裂最容易中）；暂未修，遇上了先调小该值。
