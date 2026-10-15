# JPOV 资产生成「外部工具环境」——统一管理（Mantaflow + Blender + pyopenvdb）

> 位置：`tools/jpov/genVolume/env/`
>
> 目的（Danis 2026-10-16）：「**仿真器放进容器环境，像被『安装』了那样管理**；
> Blender 等插件也**统一管理起来，不要散着**」。

## 为什么要有这个目录

JPOV 的这些 **dev-time 外部工具**（不在运行时/构建里，只在离线出资产时用）：

| 工具 | 用途 | 谁在用 |
|---|---|---|
| **Mantaflow 0.13** | 火/烟/家族体积仿真（.npz） | `fire_scene*.py` / `smoke_scene.py` / `fire_family_scene.py` |
| **Blender 4.x + Sapling 插件** | 出植被 GLB（形状 + 贴图） | `../vegetation/build_tree.py` / `render_hero.py` |
| **pyopenvdb** | 读 VDB（EmberGen 等） | `vdbseq_to_jpovvol.py` / `vdb_to_jpovvol.py` |
| **xvfb** | 无显示器跑 JPOV viewer | 各种 `--capture` |

**以前这些散在 `/james_pm/fire_hero/mantaflow/`（68MB 已编译 build）+ 系统 apt 里**，随机器漂移、无法复现。
现在**统一到本目录**：一份 `Dockerfile`（推荐）+ 一份 `setup_dev_env.sh`（裸机/容器内手装）。

**统一 = 一处定义版本、一处装、一处查。** 新加外部工具就往这儿加，别在别处再建一套。

## 用法

### A. 容器（推荐，可复现）

```bash
# 在仓库根构建（约 20~40 min，取决于机器；Mantaflow 编译占大头）
docker build -t jpov-genvolume tools/jpov/genVolume/env

# 跑转换器（把仓库 + 数据挂进来）
docker run --rm -v "$PWD":/work -w /work/tools/jpov/genVolume jpov-genvolume \
    python3 -m unittest discover -s . -p 'test_*.py' -v

# 跑仿真（manta 需在其 waveletNoiseTile.bin 目录运行 → 镜像里已用 manta-run 包好）
docker run --rm -v "$PWD":/work -w /work/tools/jpov/genVolume jpov-genvolume \
    manta-run RES=96 FRAMES=96 OUT=/tmp/out fire_scene_stable.py
```

### B. 裸机 / 容器内手装

```bash
bash tools/jpov/genVolume/env/setup_dev_env.sh
```

装好后自检：

```bash
python3 -c "import numpy, jpvol; print('numpy ok')"
python3 -c "import pyopenvdb; print('pyopenvdb ok')"
manta --help    # 或 manta-run（包装器，自动切到 waveletNoiseTile.bin 目录）
blender --version
```

## Blender 插件（Sapling）管理说明

- **Sapling Tree Gen 是 Blender 自带插件**（`scripts/addons/add_curve_sapling/`），**无需单独安装**；
  由 `blender -b --python-expr "import bpy; bpy.ops.preferences.addon_enable(module='add_curve_sapling')"` 启用。
- 我们只用它**离线出 GLB**（见 `../vegetation/README.md`）；**Blender 不链接进 JPOV 运行时**。
- **GPL 注意**：Blender 是 GPL 独立应用。**别把 Blender 打进产品分发包**；做产品镜像时用 **multi-stage**
  把 Blender 留在 builder 阶段，final 阶段只放 JPOV 产物 + 非 GPL 运行时。

## 已知坑

- Mantaflow 编译在 Ubuntu 24.04 会有 `-lHalf` 链接问题（Half 已并入 Imath）→ `setup_dev_env.sh` 里建了软链。
- `manta` 运行目录必须含 `waveletNoiseTile.bin`（镜像内提供 `manta-run` 包装器）。
- `python3-openvdb` 走 apt（Ubuntu 24.04 是 10.0.1）；若走 pip 需自编，别混用。
