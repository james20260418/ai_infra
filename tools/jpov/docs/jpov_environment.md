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
| `third_party/distdir/` | `protobuf_deps()` 那 6 个 `http_archive` 的原始 tarball（断网时 Bazel 从这里取，见 §4） |
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

## 4. 第三方下载：全部走仓库自带 distdir（断网可构建）

`WORKSPACE` 自身已无 `http_archive`：

- ✅ `gtest` → `local_repository("third_party/googletest-release-1.11.0")`
- ✅ `com_github_gflags_gflags` → 整条删除（glog 是预编译 .a，构建期不需要 gflags；全仓 0 处引用）

`third_party/protobuf-3.14.0-rc3/protobuf_deps.bzl` 里仍有 7 个 `http_archive`，
其中**实测只有 5 个会被 fetch**，它们的**原始 tarball 已入库** `third_party/distdir/`
（压缩包原样、不展开源码，git 只涨 ~3.0MB）：

| archive | 版本 | distdir 文件名 | tar.gz |
|---|---|---|---|
| bazel_skylib | 1.0.2 | `bazel-skylib-1.0.2.tar.gz` | 31K |
| zlib | 1.2.11 | `v1.2.11.tar.gz` | 630K |
| rules_cc | 818289e | `818289e5613731ae410efb54218a4077fb9dbb03.tar.gz` | 112K |
| rules_proto | 4029855 | `40298556293ae502c66579620a7ce867d5f57311.tar.gz` | 12K |
| rules_python | 4b84ad2 | `4b84ad270387a7c439ebdccfd530e2339601ef27.tar.gz` | 2.3M |

声明了但**当前构建从不 fetch**、因此没入库的 2 个（用「移走 tarball + 空 output_base +
`--experimental_repository_disable_download` + `cquery deps(//tools/jpov/...)`」逐个验证）：

| archive | 原因 |
|---|---|
| `six` 1.12.0 | Bazel 从未下载过该 repo（repository cache 里就没有） |
| `rules_java` 981f06c | 移走它离线 cquery 仍 415 targets 全通过；只在 java toolchain 解析时用，而本仓库没有 java 目标、coverage 也被 §4.1 绕开 |

规则：

- 文件名必须是**上游 URL 的 basename**（Bazel 按 basename 在 distdir 里找），
  内容 sha256 必须与 `protobuf_deps.bzl` 声明一致（`sha256sum third_party/distdir/*` 可核对）。
- 开关在 `.bazelrc`：`common --distdir=third_party/distdir`（相对 workspace 根）。
- 加新依赖时：把上游 URL 的 tarball 原样拷进来即可，不要改内容、不要重命名。
  ⚠️ distdir 是**按 basename 命中**的：如果将来两个不同仓库恰好有同名 basename
  （`v1.2.11.tar.gz` 这种没有项目名的尤其危险），先命中的那个会被拿去做 sha256 校验，
  对不上就报下载失败。真遇到时改名是没用的（Bazel 只认 URL basename），只能改用
  `new_local_repository` 或调整依赖。
- **对账脚本**：`python3 third_party/distdir/verify.py`（退出码 0 = OK）。检查
  需要的 tarball 齐全且 sha256 与 bzl 一致、无孤儿文件。**升级 protobuf_deps / 换 Bazel
  版本后必须跑一次**——distdir 失效是静默的（有网时照常绿，断网才炸），CI 有网测不出来。

### 4.1 另外 237MB 的坑：`cc_test` 的 coverage_report_generator

`cc_test` 有个隐式属性 `coverage_report_generator`，默认指向
`@remote_coverage_tools//:coverage_report_generator`——那是个 **java_binary**。
于是只要**分析到任何一个 test 目标**（`bazel build //tools/jpov/...` 也算，里面全是 cc_test），
Bazel 就要解析 java toolchain，从而下载：

| repo | 包 |
|---|---|
| `remote_java_tools` | java_tools-v11.7.1.zip（42M） |
| `remote_java_tools_linux` | java_tools_linux-v11.7.1.zip（1.3M） |
| `remotejdk11_linux` | Zulu JDK 11 tar.gz（194M，解压 320M） |

本仓库从不跑 `bazel coverage`，所以 `.bazelrc` 里把生成器换成一个 bazel 自带的
shell 脚本 target（`coverage_support`）——它实际是个 filegroup，只是个占位：

```
build --coverage_report_generator=@bazel_tools//tools/test:coverage_support
```

⇒ 干净机器/断网上这 237MB 一个字节都不用下。

注意这里的取值是个 **filegroup（一组 .sh），不是可执行程序** —— 它只是占位，
正常 build/test 只用到「这个属性指向哪个 label」这一点，不会执行它。

**真要用 coverage 时**（实测可用，2026-11-06）：把生成器恢复默认，并指定远程 JDK
（容器里没有系统 JDK，`local_jdk` 会报 `Cannot find Java binary bin/java ... nosystemjdk`）：

```bash
bazel coverage //tools/jpov/test/sp_ui:sp_ui_text_edit_test --combined_report=lcov \
  --instrument_test_targets \
  --coverage_report_generator=@bazel_tools//tools/test:coverage_report_generator \
  --java_runtime_version=remotejdk_11 --tool_java_runtime_version=remotejdk_11
# → bazel-out/_coverage/_coverage_report.dat（实测 6507 字节 / 385 条 DA:）
```

不指定 `--instrument_test_targets` 时报告是 0 字节（`There was no coverage found`），
因为默认只插桩被测库、不插桩测试本身。这套命令会重新引入那 237MB 下载。

**验证（2026-11-06，空 output_base + `--experimental_repository_disable_download`）**：
`bazel build //tools/jpov/...` → 全绿（旧基线 1741 actions / 78s；rebase 到含 #193 fog 重构的
main 合并后 559 actions，均含 disk cache）；
`bazel test //tools/jpov/test/sp_ui:all //tools/jpov/test/strip:all` → 7 tests / 68 cases 全绿。
唯一不在此列的是 Bazel 自带的 `@platforms` / `@bazel_tools` / `local_config_*`——它们在 install_base 里，本来就不联网。

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
