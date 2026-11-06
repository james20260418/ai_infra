# third_party/distdir — 离线构建用的第三方压缩包

这里是 **`third_party/protobuf-3.14.0-rc3/protobuf_deps.bzl`** 里那 6 个
`http_archive` 的**原始 tarball**（原样拷贝，不解压、不修改）。

`WORKSPACE` 里没有为它们写 `local_repository`：Bazel 仍然声明 `http_archive`
（保留 `sha256` 校验和 `strip_prefix` 语义），只是通过
`.bazelrc` 的 `common --distdir=third_party/distdir` 优先从这里取包。
命中 distdir 就完全不联网。

## 为什么是压缩包而不是源码树

| 方案 | git 增长 | 工作区 | 备注 |
|---|---|---|---|
| 源码树 vendoring（像 gtest 那样） | ~2.6MB（pack 后） | +13MB | 要改 `build_file`（zlib/six 用的是 protobuf 自带的 BUILD） |
| **distdir 压缩包（本目录）** | **~3.1MB** | 0 | 上游定义一字不改，sha256 校验照旧，升级只换文件 |

## 文件清单

| 文件 | 来源 archive | 上游 URL 的 basename | sha256 |
|---|---|---|---|
| `bazel-skylib-1.0.2.tar.gz` | `bazel_skylib` | `bazel-skylib-1.0.2.tar.gz` | `97e70364…` |
| `v1.2.11.tar.gz` | `zlib` | `v1.2.11.tar.gz`（zlib 1.2.11） | `629380c9…` |
| `818289e5613731ae410efb54218a4077fb9dbb03.tar.gz` | `rules_cc` | `818289e…tar.gz` | `9d48151e…` |
| `981f06c3d2bd10225e85209904090eb7b5fb26bd.tar.gz` | `rules_java` | `981f06c…tar.gz` | `f5a3e477…` |
| `40298556293ae502c66579620a7ce867d5f57311.tar.gz` | `rules_proto` | `4029855…tar.gz` | `aa1ee192…` |
| `4b84ad270387a7c439ebdccfd530e2339601ef27.tar.gz` | `rules_python` | `4b84ad2…tar.gz` | `e5470e92…` |

完整 sha256 以 `protobuf_deps.bzl` 为准，随时可核对：

```bash
sha256sum third_party/distdir/*
```

`six`（1.12.0）**没有入库**：当前构建完全不引用它（Bazel 从未下载过这个 repo）。

## 加新依赖 / 升级版本

1. 从 `protobuf_deps.bzl`（或新写的 `http_archive`）里抄出 `urls` 和 `sha256`；
2. 把上游 tarball **原样**放到本目录，**文件名必须等于 URL 的 basename**
   （Bazel 就是按 basename 在 distdir 里找；改成 `rules_cc-<sha>.tar.gz` 这种好名字会找不到）；
3. `sha256sum` 核对一遍；
4. 断网验证：`bazel --output_base=/tmp/ob_off build --experimental_repository_disable_download //tools/jpov/...`。

## 不在本目录、也不需要联网的东西

- `@platforms`、`@bazel_tools`、`local_config_*`：随 Bazel 安装包（`install_base`）提供。
- `remote_java_tools` / `remotejdk11_linux`（237MB）：本来是 `cc_test` 的
  `coverage_report_generator` 那个 java_binary 连带拉下来的，已被
  `.bazelrc` 的 `build --coverage_report_generator=@bazel_tools//tools/test:coverage_support`
  绕开，见 `tools/jpov/docs/jpov_environment.md` §4.1。
