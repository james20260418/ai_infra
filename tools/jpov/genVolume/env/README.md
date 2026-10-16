# genVolume 外部工具环境

> 位置：`tools/jpov/genVolume/env/`
>
> **环境用 Docker 镜像管理。** 本目录只放 **镜像定义 `Dockerfile`** + **依赖清单 `dependency.txt`**，
> 不放任何「在宿主机装环境」的脚本。

## 为什么要有这个目录

JPOV 的这几个 **dev-time 外部工具**（不在运行时/构建里，只在离线出资产时用）：

| 工具 | 用途 | 谁在用 |
|---|---|---|
| **Mantaflow 0.13** | 火/烟/家族体积仿真（.npz） | `fire_scene*.py` / `smoke_scene.py` / `fire_family_scene.py` |
| **Blender 4.x + Sapling 插件** | 出植被 GLB | `../vegetation/build_tree.py` / `render_hero.py` |
| **pyopenvdb** | 读 VDB（EmberGen 等） | `vdb_to_jpovvol.py` |
| **xvfb** | 无显示器跑 JPOV viewer | 各种 `--capture` |

统一由**一个镜像**提供（版本可复现），不要在宿主机上各自手装。

## 用法

```bash
# 构建（首次含 Mantaflow 源码编译，约 20~40 min）
docker build -t jpov-genvolume tools/jpov/genVolume/env

# 跑单测（把仓库挂进来）
docker run --rm -v "$PWD":/work -w /work/tools/jpov/genVolume jpov-genvolume \
    python3 -m unittest discover -s . -p 'test_*.py' -v

# 跑仿真（manta 需在其 waveletNoiseTile.bin 目录运行 → 镜像里已用 manta-run 包好）
docker run --rm -v "$PWD":/work -w /work/tools/jpov/genVolume jpov-genvolume \
    manta-run RES=96 FRAMES=96 OUT=/tmp/out fire_scene_stable.py
```

## Blender 插件（Sapling）说明

- **Sapling Tree Gen 是 Blender 自带插件**（`scripts/addons/add_curve_sapling/`），**无需单独安装**；
  由 `blender -b --python-expr "import bpy; bpy.ops.preferences.addon_enable(module='add_curve_sapling')"` 启用。
- 只用它**离线出 GLB**（见 `../vegetation/README.md`）；**Blender 不链接进 JPOV 运行时**。
- **GPL 注意**：Blender 是 GPL 独立应用。**别把 Blender 打进产品分发包**；做产品镜像时用 **multi-stage**
  把 Blender 留在 builder 阶段，final 阶段只放 JPOV 产物 + 非 GPL 运行时。
