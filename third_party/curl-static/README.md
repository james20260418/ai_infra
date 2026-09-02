# curl-static — 裁剪版静态 libcurl（vendor 自包含）

Linux 静态 HTTP/HTTPS 客户端，供 JPOV gen3d 工具调 tripo3d REST API 使用。
与 `glfw-static` / `glog-static` / `opencv-static` 同款：把编译好的 `.a`
直接 commit 进仓库，构建机无需装任何 dev 包，CI 干净 runner 也可复现。

## 目录内容

| 文件 | 说明 | 版本 |
|---|---|---|
| `libcurl.a` | 裁剪版 curl（仅 http/https/file/ftp + OpenSSL TLS） | curl 8.5.0 |
| `libssl.a` / `libcrypto.a` | OpenSSL 静态库 | OpenSSL 3.0.13 |
| `libz.a` | zlib 静态库 | zlib 1.3 |
| `include/curl/*.h` | libcurl 公共头 | curl 8.5.0 |

> libssl/libcrypto/libz 是 libcurl 编译时的依赖，一并 commit 是为了让
> target **自包含**：CI（ubuntu-24.04 runner，未装 libssl-dev）上
> `bazel build //...` 也能静默链接成功，不依赖构建机的 apt 系统库。

## 为什么是裁剪版 libcurl

直接用 Ubuntu 的 apt `libcurl.a` 静态链接会失败：
系统 curl 编译时 gssapi/kerberos + nghttp2 + libpsl + brotli + zstd 全开，
静态链时喷一地 `undefined reference`（`gss_*`、`nghttp2_*` 等符号）。

裁剪后只留必需能力，符号干净，静态可执行仅 ~7MB（含 OpenSSL+zlib）。

## 可复现 / 升版配方（curl 8.5.0）

在有 `libssl-dev zlib1g-dev` 的机器上：

```bash
# 1. 下载 + 解压
curl -sL -o /tmp/curl-8.5.0.tar.gz https://curl.se/download/curl-8.5.0.tar.gz
tar xzf /tmp/curl-8.5.0.tar.gz -C /tmp

# 2. 裁剪 configure（关闭全部非必需协议/特性）
cd /tmp/curl-8.5.0 && mkdir build && cd build
../configure \
  --disable-shared --enable-static \
  --without-libidn2 --without-librtmp --without-libpsl \
  --without-brotli --without-zstd --without-nghttp2 \
  --without-nghttp3 --without-ngtcp2 --without-libssh2 \
  --disable-ldap --disable-ldaps --disable-rtsp --disable-dict \
  --disable-telnet --disable-tftp --disable-pop3 --disable-imap \
  --disable-smtp --disable-gopher --disable-mqtt --disable-manual \
  --disable-unix-sockets --disable-cookies --disable-alt-svc \
  --disable-hsts \
  --with-openssl --with-zlib

# 3. 编译
make -j$(nproc)
# 产物: build/lib/.libs/libcurl.a

# 4. 替换本目录 libcurl.a，并同步 include/curl/*.h（从源码 include/curl 拷）
cp build/lib/.libs/libcurl.a /path/to/third_party/curl-static/libcurl.a
cp ../include/curl/curl.h curlver.h easy.h mprintf.h multi.h \
   /path/to/third_party/curl-static/include/curl/
```

升版时只需换 curl 版本号重跑上面即可。OpenSSL / zlib 若有版本升级诉求，
从构建机 `/usr/lib/x86_64-linux-gnu/lib{ssl,crypto,z}.a` 拷入替换即可
（注意 libcurl 编译期 --with-openssl/--with-zlib 的版本建议与之一致）。

## 使用

BUILD 已把四个 `.a` 按依赖顺序收进 `cc_library(name="curl")`，codebase 内
`deps = ["//third_party/curl-static:curl"]` 即可。链接顺序敏感：
`libcurl.a → libssl.a → libcrypto.a → libz.a`，BUILD srcs 已保证顺序。

> ⚠️ 仅 Linux host（`target_compatible_with` 限 linux/x86_64）。Windows 交叉
> 编译不打 gen3d 分支的 curl target，若未来要给 MinGW 也请单独 vendoring
> 一份 windows 版 .a，勿把本 linux .a 交叉链到 .exe。
