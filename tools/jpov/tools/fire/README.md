# JPOV 粒子火/烟 — flipbook 纹理离线烘焙

给「粒子火」场景（`jpov_fire_fog_viewer`）产出**真火 / 真烟**的 flipbook 序列帧贴图。
思路：Mantaflow 烘体积（density+temp）→ 逐帧 emission-absorption 积分成 2D 帧 → 拼成
N×N 序列图 PNG。引擎运行期只读 PNG（`assets/fire/*.png`，build 脚本拷到 exe 旁 `textures/`）。

设计：`tools/jpov/docs/jpov_particle_fire_design.md`。
体数据格式（JPVOL1/2）见 `tools/jpov/src/fire_fog/fire_volume.h`。

## 依赖
- `python3` + `numpy`
- Mantaflow standalone 0.13（烘体积用；已编译版在 `/james_pm/fire_hero/mantaflow/build/manta`。
  重编步骤见 `/james_pm/fire_hero/README.md`）

## 工作流

```bash
# 1) 烘体积（火 或 烟）；输出每帧 .npz 到 out 目录
cd <workdir-with-waveletNoiseTile.bin>          # manta 需在其 cwd 找 waveletNoiseTile.bin
RES=96 FRAMES=96 OUT=/tmp/smoke_out manta /path/to/tools/fire/smoke_scene.py
#   火：tools/fire/fire_scene_stable.py（稳定燃烧）；旋钮 NOISE_SCALE/VORT_*/SRC_W/BUOY_*

# 2) npz 序列 → .jpovvol（<1GB，取稳态段 + 尾部交叉淡化做无缝循环 + u8）
python3 /path/to/tools/npzseq_to_jpovvol.py /tmp/smoke_out /tmp/smoke.jpovvol \
        --skip 24 --frames 64 --blend 8 --dtype u8

# 3) .jpovvol → flipbook PNG（8×8=64 帧；格内留透明内边距、按 bbox 裁剪）
python3 make_flipbook.py smoke /tmp/smoke.jpovvol ../assets/fire/smoke_flipbook.png 8
python3 make_flipbook.py fire  /path/fire_stable.jpovvol ../assets/fire/fire_flipbook.png 8
```

## 坑
- `NoiseField.valScale = 0` 会把密度源也归零（源为 0 → 整场空）；烟/火都要给**非零** valScale。
- `manta` 必须在其 `waveletNoiseTile.bin` 所在目录运行。
- flipbook 每格需**透明内边距**（`make_flipbook.py` 的 `cell` 放大系数），否则广告牌露硬边。
- 颜色：火沿温度→黑体色；烟输出白色 + alpha，由引擎 `color_over_life` 染黑灰。
