#!/usr/bin/env python3
"""校验 third_party/distdir 与 protobuf_deps.bzl 是否对得上。

为什么需要这个脚本：
    distdir 靠「文件名 = 上游 URL 的 basename + 内容 sha256 匹配」命中。一旦
    protobuf_deps.bzl 升级版本 / 换 Bazel 版本导致 sha256 变了，distdir 会**静默
    失效**（联网时照常构建成功，断网时才炸，而且错误信息只会说 "Failed to
    download ..."）。CI 有网，测不出来，所以用这个脚本在 PR 阶段对账。

检查项：
    1. 每个「当前构建确实会用到」的 archive：distdir 里有同名文件，且 sha256 一致；
    2. distdir 里没有孤儿文件（对不上任何声明）；
    3. 声明了但当前构建不引用的（SKIP 列表）：文件存在时给 warning，不报错。

用法：
    python3 third_party/distdir/verify.py         # 退出码 0 = OK，1 = 有问题
"""

from __future__ import annotations

import hashlib
import os
import re
import sys
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEPS_BZL = os.path.join(
    REPO_ROOT, "third_party", "protobuf-3.14.0-rc3", "protobuf_deps.bzl"
)

# 声明了、但当前构建（纯 C++，不解析 java toolchain）从不 fetch 的。
# 2026-11-06 用「移走 tarball + 空 output_base + --experimental_repository_disable_download
# + cquery deps(//tools/jpov/...)」逐个验证过：
#   six        —— Bazel 从未下载过该 repo（连 repository cache 里都没有）
#   rules_java —— 移走它，离线 cquery 仍然 415 targets 全通过
SKIP = {
    "six": "当前构建从不引用（Bazel 从未下载过）",
    "rules_java": "只在 java toolchain 解析时用；本仓库没有 java 目标，且 coverage 已另作处理",
}


def parse_deps(path: str) -> list[dict[str, str]]:
    """从 protobuf_deps.bzl 里抠出 (name, sha256, urls[0]) —— 只认 http_archive 块。"""
    text = open(path, encoding="utf-8").read()
    out: list[dict[str, str]] = []
    for m in re.finditer(r"http_archive\((.*?)\n\s*\)", text, re.S):
        block = m.group(1)
        name = re.search(r'name\s*=\s*"([^"]+)"', block)
        sha = re.search(r'sha256\s*=\s*"([0-9a-f]{64})"', block)
        url = re.search(r'urls\s*=\s*\[\s*"([^"]+)"', block)
        if name and sha and url:
            out.append({"name": name.group(1), "sha256": sha.group(1), "url": url.group(1)})
    return out


def sha256_of(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main(argv: list[str]) -> int:
    deps_bzl = argv[1] if len(argv) > 1 else DEPS_BZL  # 参数只为测试用
    if not os.path.exists(deps_bzl):
        print(f"FAIL 找不到 {deps_bzl}")
        return 1

    declared = parse_deps(deps_bzl)
    if not declared:
        print(f"FAIL 没能从 {deps_bzl} 解析出任何 http_archive（格式变了？）")
        return 1

    problems: list[str] = []
    warnings: list[str] = []

    # distdir 只按 URL basename 找文件（Bazel 5 没有 sha256 兜底，实测），
    # 两个 archive 撞 basename 时永远只有一个能被命中，必须报出来。
    seen: dict[str, str] = {}
    for dep in declared:
        fn = os.path.basename(urllib.parse.urlparse(dep["url"]).path)
        if fn in seen:
            problems.append(
                f"basename 冲突：{seen[fn]} 与 {dep['name']} 的 URL 都叫 {fn}（distdir 无法区分）"
            )
        else:
            seen[fn] = dep["name"]
    expected_files: set[str] = set()
    known_files: set[str] = set()   # 声明过的全部（含 SKIP），用于孤儿判定

    for dep in declared:
        filename = os.path.basename(urllib.parse.urlparse(dep["url"]).path)
        path = os.path.join(HERE, filename)
        state = "skip" if dep["name"] in SKIP else "need"
        known_files.add(filename)

        if not os.path.exists(path):
            msg = f"{dep['name']:<16} {filename:<52} 缺文件"
            if state == "need":
                problems.append(msg + "（需要！断网构建会失败）")
            else:
                warnings.append(msg + f"（{SKIP[dep['name']]}，可以没有）")
            continue

        actual = sha256_of(path)
        ok = actual == dep["sha256"]
        line = f"{dep['name']:<16} {filename:<52} {'sha256 OK' if ok else 'sha256 MISMATCH'}"
        if not ok:
            problems.append(
                f"{line}\n    expected {dep['sha256']}\n    actual   {actual}"
            )
        elif state == "need":
            expected_files.add(filename)
        else:
            warnings.append(f"{line}（{SKIP[dep['name']]}，可以删掉省点体积）")

    for name in sorted(os.listdir(HERE)):
        if name.endswith((".tar.gz", ".zip")) and name not in known_files:
            problems.append(f"孤儿文件 {name}（distdir 里没有对应的「需要」声明）")

    for w in warnings:
        print(f"WARN  {w}")
    for p in problems:
        print(f"FAIL  {p}")

    if problems:
        print(f"\n{len(problems)} 个问题；distdir 与 protobuf_deps.bzl 不一致。")
        return 1

    print(f"\nOK  {len(expected_files)} 个需要的 tarball 全部就位且 sha256 一致。")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
