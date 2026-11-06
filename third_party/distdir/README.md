# third_party/distdir — 离线构建用的第三方压缩包

这里是 **`third_party/protobuf-3.14.0-rc3/protobuf_deps.bzl`** 里那些 `http_archive`
的**原始 tarball**（原样拷贝，不解压、不修改）。

`WORKSPACE` 里没有为它们写 `local_repository`：Bazel 仍然声明 `http_archive`
（保留 `sha256` 校验和 `strip_prefix` 语义），只是通过
`.bazelrc` 的 `common --distdir=third_party/distdir` 优先从这里取包。
命中 distdir 就完全不联网。

## 为什么是压缩包而不是源码树

| 方案 | git 增长 | 工作区 | 备注 |
|---|---|---|---|
| 源码树 vendoring（像 gtest 那样） | ~2.6MB（pack 后） | +13MB | 要改 `build_file`（zlib/six 用的是 protobuf 自带的 BUILD） |
| **distdir 压缩包（本目录）** | **~3MB** | 0 | 上游定义一字不改，sha256 校验照旧，升级只换文件 |

## 文件清单（6 个，构建确实会 fetch 的）

| 文件 | 来源 archive | sha256 |
|---|---|---|
| `bazel-skylib-1.0.2.tar.gz` | `bazel_skylib` 1.0.2 | `97e70364…` |
| `v1.2.11.tar.gz` | `zlib` 1.2.11 | `629380c9…` |
| `818289e5613731ae410efb54218a4077fb9dbb03.tar.gz` | `rules_cc` | `9d48151e…` |
| `981f06c3d2bd10225e85209904090eb7b5fb26bd.tar.gz` | `rules_java` | `f5a3e477…` |
| `40298556293ae502c66579620a7ce867d5f57311.tar.gz` | `rules_proto` | `aa1ee192…` |
| `4b84ad270387a7c439ebdccfd530e2339601ef27.tar.gz` | `rules_python` | `e5470e92…` |

**声明了但没入库的只有 1 个**（2026-11-06 验证：Bazel 从未下载过该 repo，
repository cache 里都没有）：

| archive | 为什么不入库 |
|---|---|
| `six` 1.12.0 | 当前构建从不引用 |

> ⚠️ `rules_java` **必须留在这里**，别学 `six` 删掉。
> 本地（容器没有 JDK）去掉它离线也能全绿，但那是因为 `@local_jdk` 解析失败、
> Bazel 跳过了注册的 toolchain；**GitHub runner 预装 JDK**，`@local_jdk` 能解析，
> 它注册的 `runtime_toolchain_definition` 依赖 `@rules_java//java`，缺了就红。
> 这个坑真实踩过：PR #254 的 CI 两条 job 因此各红了一次。

> 以后真要启用 Java（`java_library` / `java_proto_library` / `bazel coverage`），
> 光有 `rules_java` 不够，还要 `remote_java_tools`(42M) + Zulu JDK(194M)。

## 加新依赖 / 升级版本

1. 从 `protobuf_deps.bzl`（或新写的 `http_archive`）里抄出 `urls` 和 `sha256`；
2. 把上游 tarball **原样**放到本目录，**文件名必须等于 URL 的 basename**
   （Bazel 就是按 basename 在 distdir 里找；改成 `rules_cc-<sha>.tar.gz` 这种好名字会找不到）；
3. 跑 `python3 third_party/distdir/verify.py` 对账；
4. 断网验证：`bazel --output_base=/tmp/ob_off build --experimental_repository_disable_download //tools/jpov/...`。

## 为什么有个 `verify.py`

distdir 失效是**静默**的：`protobuf_deps.bzl` 升级 / 换 Bazel 版本后 sha256 一变，
distdir 就命中不了，联网时照常构建成功，只有断网才炸，而且报错只会说
`Failed to download repo ...`。CI 有网，测不出来。

所以用脚本对账（PR 阶段跑一次即可，纯本地无网络）：

```bash
python3 third_party/distdir/verify.py    # 退出码 0 = OK
```

它检查：需要的 tarball 是否齐全且 sha256 与 bzl 一致 / distdir 里有没有孤儿文件 /
声明了但可以不要的那两个是否被误加回来。

## 不在本目录、也不需要联网的东西

- `@platforms`、`@bazel_tools`、`local_config_*`：随 Bazel 安装包（`install_base`）提供。
- `remote_coverage_tools` / `remote_java_tools` / `remotejdk11_linux`（共 237MB）：
  本来是 `cc_test` 的 `coverage_report_generator`（一个 java_binary）连带拉下来的，
  已被 `.bazelrc` 的 `build --coverage_report_generator=@bazel_tools//tools/test:coverage_support`
  绕开，见 `tools/jpov/docs/jpov_environment.md` §4.1。
