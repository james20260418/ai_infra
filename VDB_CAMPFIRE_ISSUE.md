# VDB 接入手记：EmberGen campfire 在 JPOV 里"躺着/乱码"问题（交接单）

> 2026-10-15 深夜 ｜ 起因：手工烘的火不好看，改用 **JangaFX（EmberGen）免费 CC0 VDB**（Small Camp Fire）。
> 目标：把 VDB 序列转成引擎的 `.jpovvol`，在 `jpov_volume_fire_viewer` 的「原型火纹理」模式里**正常立着**渲染。
> **结论（供接手）**：纹理本身没问题、渲染管线没问题、合成体也没问题，**问题锁定在 VDB→JPVOL2 的转换环节**；本文件汇总代码位置、已证事实、可疑点、复现命令。

---

## 1. 数据来源

- 资源：JangaFX EmberGen 免费 VDB，**CC0**（`https://jangafx.com/software/embergen/download/free-vdb-animations`）。
- 已下载解压：`/tmp/vdbtry/smallCampfire/smallCampfireVDB/smallCampfire_%04d.vdb`（200 帧）。
- 每帧两个 grid：**`density`**（烟/soot）、**`flames`**（火焰/反应）。无 temperature。
- grid `transform`：**单位基向量 + 平移**（无旋转）；`voxelSize=(1,1,1)`。索引轴 = 世界轴。
  - 例：flames index bbox (35,41,7)-(131,129,83)；density index bbox (22,16,2)-(134,135,187)。

## 2. 代码位置（都在 `ai_infra_4` 与 `fire_hero`）

| 环节 | 文件 | 关键点 |
|---|---|---|
| **VDB → JPVOL2 转换**（可疑核心） | `/james_pm/fire_hero/tools/vdbseq_to_jpovvol.py` | 用 `pyopenvdb` 读 grid → 稠密数组 `[F][z][y][x]` → 下采样 → 旋转/翻转 → 归一化 → 写 JPVOL2 + sidecar |
| JPVOL2 格式规范 | `/james_pm/fire_hero/tools/npzseq_to_jpovvol.py`（docstring） | 头 40B：magic JPVOL2、ver、nx,ny,nz、nframes、channels、dtype、reserved；数据 = 帧内 **x 最快→y→z**，帧间顺序 |
| **JPVOL2 加载到 GL** | `tools/jpov/src/volume_manager.cc` | `GL_TEXTURE_2D_ARRAY`：`width=nx`、`height=ny*nz`、`layers=nframes`、`GL_RG`；逐帧 `glTexSubImage3D` |
| **体渲染采样** | `tools/jpov/src/volume_fire/volume_fire_renderer.h` | `ToLocal(p)=(p-uBoxMin)/(uBoxMax-uBoxMin)`；`SampleVol(p)`：`V=(iz+y)/nz`，`slice=uFrame` |
| 原型查看模式 | `tools/jpov/demo/volume_fire_viewer_app.h` | `ProtoBoxForIndex()`：盒 = `dims × voxel_m`（真实尺度）；`DrawAxes()` 画红+X/绿+Y/蓝+Z |
| sidecar 解析 | `tools/jpov/demo/jpov_volume_fire_viewer.cc` | 读 `<name>.jpovvol.json` 的 `segments`/`voxel_m` |

**约定**：引擎里 `local.y` = 世界 **+Y = 上**。`SampleVol` 的 `p.y` 走纹理行（`(iz+y)/nz`），`p.z` 走 slice。

## 3. 现象

- campfire 转出后在 JPOV「原型火纹理」里**始终"躺着"**（用户语：火苗永远指向 -Z），**一次都没立起来过**。
- 对该纹理做**四向旋转**（none / 绕X+90 / 绕Y+90 / 绕Z+90）时：**部分方向直接"满天星乱码"**（下面两张）。

## 4. 已证实（排除项）

1. **纹理清白**：用 OpenVDB 官方 `vdb_render`（`-name flames`）在同机渲，火焰**形态连贯、稳定不抖**（用户确认）。→ 数据本身没问题。
2. **渲染管线清白**：**我们自行烘焙的** `.jpovvol`（jet/area/pool/transient）在 JPOV 里**都正常立着、不抖**（用户确认）。
3. **引擎轴映射清白**：造了一个**合成 `.jpovvol`**（一根沿 `[z][y][x]` 的 **y 轴**的竖柱 + 一根沿 x 的横柱），在 JPOV 里渲出**竖柱立着**（用户在自己的 viewer 复现确认）。→ 引擎把 `[z][y][x]` 的 **y 当"上"** 是对的。
4. **转换保真**：单帧 (frame 100) 往返比对，转出的 `.jpovvol [z][y][x]` 与 `flames` 稠密数组**逐体素一致（max|diff|=0.002）**。→ 转换本身无镜像/转置（在"不旋转"时）。
5. **transform 无旋转**：grid 的基向量是单位阵（见 §1）。

## 5. 可疑点（接手请重点看这里）

- **A. "接错通道/轴"**：虽然 §4.4 往返一致，但**读取与写入都是同一份我自己写的代码** —— 若我对 `[z][y][x]` 的理解与引擎/JPVOL2 规范在**同一处**理解偏了，往返仍会通过。**建议**：拿一个"官方 `vdb_render` 里明确竖着"的单帧，走**完整转换链**（脚本 → JPVOL2 → 引擎），与 `vdb_render` 结果**并排**核对每根轴。
- **B. `flames` 的轴向**：`flames` 加权 std(x,y,z)≈(12.0, 17.3, 16.9) → **近各向同性**（略偏 Y），所以"肉眼看着像竖/躺"都可能是错觉。**需要用"明确各向异性"的数据**判别（例如对 x/y/z 分别做人工拉伸再验证回路）。
- **C. 旋转实现**：曾把旋转放在**降采样之前**（`block_ds` 假定末三轴=(z,y,x)，转后变 (z,x,y) → 轴被揉 → **满天星**）。已改为**降采样之后再旋转**；但用户复查**仍见乱码**（可能：本地二进制/资产没更新，或 bug 另有其因）。**请检查旋转/写盘的轴序与 reshape 假设**。
- **D. 播放/抖动**：VDB 本身不抖（§4.1），但 JPOV 里"整团沿火苗方向抖" —— 若轴问题定了再看 `volume_fire_renderer` 的 frame 插值与 ray-march jitter seed。

## 6. 复现

```bash
# 1) 转换（无旋转）
python3 /james_pm/fire_hero/tools/vdbseq_to_jpovvol.py \
  '/tmp/vdbtry/smallCampfire/smallCampfireVDB/smallCampfire_%04d.vdb' \
  /tmp/cam.jpovvol --start 60 --end 140 --ds 2 --r-grid flames --g-grid flames --gnorm 99.5

# 2) 放进原型目录（viewer 从 exe 旁 volumes/prototypes 读）
cp /tmp/cam.jpovvol /tmp/cam.jpovvol.json \
   /james_pm/ai_infra_4/output/jpov_volume_fire_viewer/volumes/prototypes/
#    并把 area.jpovvol/.json 指向它

# 3) 跑 viewer → 勾「原型火纹理」→ 选 area
/james_pm/ai_infra_4/output/jpov_volume_fire_viewer/jpov_volume_fire_viewer

# 4) 官方参照（同一帧）
vdb_render /tmp/vdbtry/smallCampfire/smallCampfireVDB/smallCampfire_0100.vdb /tmp/ref.png \
  -name flames -res 500x500 -translate -196.5,165,-114.5 -lookat 3.5,165,-114.5
```

## 7. 环境事实

- 本机已装：`pyopenvdb`（python 绑定）、`vdb_print` / `vdb_render` / `vdb_view` / `vdb_tool` / `vdb_lod`、`libopenvdb 10.0`、`/usr/include/openvdb`、`blender`。
- viewer 的资产目录解析顺序：`<exe_dir>/volumes/prototypes` → `<exe_dir>/prototypes` → …；**建议接手时先加一行日志确认实际读到的路径**。

## 8. 一句话给接手

> **不是纹理、不是管线、不是引擎轴映射**；请把 `fire_hero/tools/vdbseq_to_jpovvol.py` 的**轴序/reshape/写盘布局**，与 `tools/jpov/src/volume_manager.cc` 的**上传布局** 和 `volume_fire_renderer.h` 的 **`SampleVol` 行/slice 映射**，**三处并排对准**（尤其 `nx/ny/nz` 与 `x最快`、`y==上` 的假设），并用"官方 `vdb_render` 竖着的帧"做端到端对照。合成体（§4.3）可作为"引擎正确"的恒真对照随时复用。
