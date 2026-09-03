# libcurl 静态库（裁剪版，vendor 自包含）+ OpenSSL 3.0.13 + zlib 1.3
#
# 背景：tripo3d / gen3d 工具需要 HTTPS REST 客户端。JPOV 不做运行时动态链
# 接，全部静态自包含集成（同 glfw-static / glog-static / opencv-static 惯例）。
#
# 为什么不直接用系统 apt libcurl.a：
#   Ubuntu 编译的 libcurl 特性全开（gssapi/kerberos + nghttp2 + psl + brotli +
#   zstd ...），静态链接会喷一地 undefined reference（gss_* / nghttp2_* ...）。
#   故从 curl 源码裁剪 configure，只保留 HTTP/HTTPS/FILE/FTP + OpenSSL TLS，
#   其余协议全 --disable，得到一个干净的静态 libcurl.a。
#
# 压缩配方（curl 8.5.0 源码，见本目录 README.md，可复现/可升版）：
#   ./configure --disable-shared --enable-static \
#     --without-libidn2 --without-librtmp --without-libpsl \
#     --without-brotli --without-zstd --without-nghttp2 \
#     --without-libssh2 --disable-ldap --disable-ldaps --disable-rtsp \
#     --disable-dict --disable-telnet --disable-tftp --disable-pop3 \
#     --disable-imap --disable-smtp --disable-gopher --disable-mqtt \
#     --disable-manual --disable-unix-sockets --disable-cookies \
#     --disable-alt-svc --disable-hsts \
#     --with-openssl --with-zlib
#
# 版本：libcurl 8.5.0 / OpenSSL 3.0.13 / zlib 1.3（Ubuntu noble）
#
# 注意：本 target 仅 Linux host 使用（gen3d_cmd 限 linux）；链接顺序敏感：
#   libcurl.a → libssl.a → libcrypto.a → libz.a（依赖在前，被依赖在后）。

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "curl",
    srcs = [
        "libcurl.a",
        "libssl.a",
        "libcrypto.a",
        "libz.a",
    ],
    hdrs = glob(["include/curl/*.h"]),
    includes = ["include"],
    linkstatic = 1,
    linkopts = [
        "-lpthread",
        "-ldl",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)
