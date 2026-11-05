# JPOV 环境依赖：镜像 / 仓库 / 按需

> 日期：2026-11-05 ｜ 适用：`ai_infra` 全项目（C++ 引擎 + 离线工具）
>
> 目标：**`/james_pm` 下不允许存在「git 未跟踪的依赖」**。
> 一个依赖要么在镜像里、要么在 git 里、要么有明确的按需安装方式（本文 + `requirements.txt` + `genVolume/env/Dockerfile`）。

---

## 0. 一句话

构建与 gold 测试只需要「**镜像 + 本仓库**」；两条**按需**链路（动捕取帧、体纹理重烘）各自在 venv / Docker 镜像里，不污染宿主。

```
镜像（编译器/图形栈/ffmpeg/python） ─┐
本仓库（third_party 源码 + 资产）     ─┼─→ bazel build/test（离线可复现，唯一例外见 §4）
venv（mediapipe 取帧）                ─┤
genVolume 镜像（Mantaflow 重烘）      ─┘
```

---

## 1. 镜像提供（不在 git，但构建/测试必需）

| 组件 | 本机版本 | 用途 | 自检 |
|---|---|---|---|
| **bazel** | **5.4.0**（`.bazelversion` 钉死；镜像另装 7.2.0 / 9.0.2） | 全部构建 | `bazel --version` |
| gcc / g++ | 13.3.0 | Linux 构建 | `g++ --version` |
| **MinGW-w64** | 13.2 | `--config=windows` 交叉编译 .exe | `x86_64-w64-mingw32-g++ --version` |
| **Xvfb** | — | 所有 GPU 测试离屏跑（`headless=true`） | `which xvfb-run` |
| **Mesa / llvmpipe** | libgl1-mesa-dri 25.2.8 | 软渲染出 gold | gold test 日志里的 `Renderer:` |
| GL 开发头 | libgl-dev / xrandr / xinerama / xcursor / xi / xrender-dev | 编译 GLFW 与渲染代码（**CI 装的是同一份清单**） | `dpkg -l libgl-dev` |
| **ffmpeg** | 6.1.1 | 动捕抽帧、viewer 出 mp4 | `ffmpeg -version` |
| ImageMagick | 6.9.12（`convert`） | 个别对比/拼图脚本 | `convert -version` |
| osslsigncode | 2.8 | Windows exe 本地签名 | `osslsigncode --version` |
| **Python** | 3.12.3 + numpy 1.26.4 / Pillow 12.3.0 / SciPy 1.11.4 | 离线出资产（genVolume 产线） | `python3 -V` |
| CJK 字体 | `fonts-droid-fallback`（DroidSansFallbackFull.ttf） | 兜底；**UI 实际用 git 里的 NotoSans** | `fc-list :lang=zh` |
| Blender | 4.0.2 | **当前管线不用**（仅 `legacy/vegetation/render_hero.py`） | `blender --version` |
| git / curl / tar | 2.43 / 8.5 | 脚本 | — |

> 镜像外的机器按这张表装齐即可构建；CI 的 `.github/workflows/ci.yml` 只装其中的 GL 头 + xvfb + mingw + python3 那几个 apt 包。

---

## 2. 仓库自带（git 跟踪，构建零网络）

| 路径 | 内容 |
|---|---|
| `third_party/googletest-release-1.11.0/` | **GoogleTest 1.11.0 源码**（2026-11-05 起随仓库携带，不再 `http_archive`） |
| `third_party/glog-static/` | glog 预编译静态库（Linux + Windows 两套 .a） |
| `third_party/glfw-static`、`glfw-mingw` | GLFW 3.3.10 静态库（Linux / MinGW） |
| `third_party/protobuf-3.14.0-rc3/` | protobuf 源码（`local_repository`） |
| `third_party/opencv-static`、`curl-static`、`ufbx-static` | 预编译静态库 |
| `third_party/stb`、`tinygltf`、`nlohmann-json`、`gl_loader-mingw` | header-only / 少量源码 |
| `tools/jpov/fonts/`、`tools/jpov/assets/`、`data/fire_vdb/campfire.jpovvol` | 运行时资产（字体、模型、体纹理小样） |

---

## 3. 按需链路（不装也能构建、能跑 gold）

| 链路 | 依赖 | 怎么装 |
|---|---|---|
| **动捕取帧**（`mocap/extract/pose_extract.py`） | mediapipe 1.2.0 + opencv-contrib 5.0.0.93 + numpy 2.5.4 + Pillow 13.0.0 | `python3 -m venv /tmp/ai_infra_py && . /tmp/ai_infra_py/bin/activate && pip install -r requirements.txt`（版本已实测，见文件头） |
| 同上：姿态模型 | `pose_landmarker_full.task`（~30MB） | 按 `tools/jpov/mocap/README.md` 下载；⚠️ 目前**无校验和** |
| **体纹理重烘**（Mantaflow / Blender+Sapling / pyopenvdb） | 见 `genVolume/env/dependency.txt` | `docker build -t jpov-genvolume tools/jpov/genVolume/env`（首次 20~40 min） |
| .vdb 读取（`vdb_to_jpovvol.py`） | pyopenvdb | 镜像/CI 用 apt `python3-openvdb`；或走上面那个 Docker 镜像 |

> 装进 venv 的理由见 `requirements.txt` 文件头：mediapipe 会拉 numpy 2 + opencv 5，
> 与镜像系统的 numpy 1.26 不能共存于同一 prefix。

---

## 4. 唯一的联网点：`protobuf_deps()`

`WORKSPACE` 自身的 `http_archive` 已清零：

- ✅ `gtest` → 改为 `local_repository("third_party/googletest-release-1.11.0")`
- ✅ `com_github_gflags_gflags` → **整条删除**（glog 是预编译 .a，构建期不需要 gflags；全仓 0 处引用）

但 `third_party/protobuf-3.14.0-rc3/protobuf_deps.bzl` 仍会 `http_archive` 这 7 个：

```
bazel_skylib / zlib / six / rules_cc / rules_java / rules_proto / rules_python
```

**影响**：干净缓存 / 新机器 / 断网环境下**首次** `bazel build` 会失败。
**解法（待办）**：把这 7 个 tarball 放进 `third_party/distdir/`，在 `.bazelrc` 加
`build --distdir=third_party/distdir`（bazel 命中 distdir 就不联网）。

---

## 5. 新依赖加在哪里（约定）

| 类型 | 位置 |
|---|---|
| C++ 三方库 | `third_party/<name>/`（源码或预编译）+ `WORKSPACE` 的 `local_repository`，**不要写 http_archive** |
| 系统工具（编译器 / Xvfb / ffmpeg…） | 本文 §1 表格 + 镜像 Dockerfile |
| Python 包 | 根目录 `requirements.txt`（**pin 版本**，并在注释写清谁在用） |
| 离线出资产的重型工具 | `tools/jpov/genVolume/env/Dockerfile` |
| 运行时资产（字体/模型/体纹理） | 仓库内 `assets/`、`data/`；**大于 ~50MB 的先问，别直接入库** |

---

## 6. 一键自检

```bash
# 构建链
bazel --version && g++ --version | head -1 && x86_64-w64-mingw32-g++ --version | head -1

# 图形栈
which xvfb-run && dpkg -l libgl1-mesa-dri | tail -1

# 离线工具
python3 -c "import numpy, PIL; print(numpy.__version__, PIL.__version__)"
ffmpeg -version | head -1

# 仓库是否干净（不应有未跟踪的依赖）
git status --porcelain
```
