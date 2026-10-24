#!/usr/bin/env python3
"""从生成配置（`configs/*.pbtxt`）调 Blender/Sapling 产出一份 shape.pbtxt。

    生成配置(proto/generate.proto) --[blender + Sapling]--> shape.pbtxt(proto/shape.proto)

用法（相对仓库根）：

    python3 tools/jpov/tools/vegetation_tool_v2/generate_shape.py \
        tools/jpov/tools/vegetation_tool_v2/configs/pine.pbtxt \
        --out output/veg_shapes/pine.pbtxt

    加 --dry-run 只打印将要执行的 blender 命令行，不真跑。

分工：
  * **Blender 不由本仓库管理**：它是镜像里的开发期工具（`/usr/bin/blender` 4.0.2），
    本脚本只负责调用它。`--blender` 可以指到别的路径（例如离线 portable 包）。
  * 建树逻辑仍在 `export_vegetation_shape.py` 里（Blender 侧，跑在 Blender 自带的
    Python 里，所以那里不能用 protobuf、不能用本文件的任何代码）。
    本脚本是**外壳**：它不做几何，只把配置翻译成那次调用的参数，并检查产物是否可用。

可测性（重要）：
  「调 Blender」本身没法单测（需要 Blender + 几分钟 + 随机数），所以这里刻意做了切分——
  `parse_config` / `build_overrides` / `build_command` / `check_shape_text` 全是**纯函数**
  （不碰磁盘、不启动进程），由 `generate_shape_test.py` 覆盖；
  真正不可测的只剩 `run_blender()` 那几行 subprocess。别把逻辑写进那里。
"""

import argparse
import os
import re
import shlex
import subprocess
import sys
import time

# 同目录下的 Blender 侧导出脚本（它自己会 import 老工具目录里的 wind_bake）。
EXPORT_SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             "export_vegetation_shape.py")

ANCHOR_MODES = ("bottom_left", "bottom_center", "bottom_right", "top_center")
R_MODES = ("original", "rotate", "random")
# Sapling 的 shape / shapeS / leafDist 是枚举，标识符是数字字符串。
SHAPE_IDS = (0, 1, 2, 3, 4, 5, 6, 7, 8, 10)
SHAPE_IDS_NO_CUSTOM = (0, 1, 2, 3, 4, 5, 6, 7, 10)

# 所有长度 4 的数组参数（下标 = 枝级）。
ARRAY_LEN = 4

# 单值参数：名字 -> 类型。名字逐字对应 Sapling operator 属性名。
SCALAR_FIELDS = {
    "scale": "float",
    "seed": "int",
    "levels": "int",
    "shape": "shape",
    "shapeS": "shapeS",
    "baseSplits": "int",
    "baseSize": "float",
    "baseSize_s": "float",
    "splitHeight": "float",
    "splitBias": "float",
    "splitByLen": "bool",
    "rMode": "rMode",
    "branchDist": "float",
    "taperCrown": "float",
    "ratio": "float",
    "ratioPower": "float",
    "minRadius": "float",
    "rootFlare": "float",
    "closeTip": "bool",
    "autoTaper": "bool",
    "leaves": "int",
    "leafDist": "leafDist",
    "leafScale": "float",
    "leafScaleV": "float",
    "leafScaleT": "float",
    "leafDownAngle": "float",
    "leafDownAngleV": "float",
    "leafRotate": "float",
    "leafRotateV": "float",
    "horzLeaves": "bool",
    "nrings": "int",
}

# 数组参数：名字 -> 元素类型。
ARRAY_FIELDS = {
    "customShape": "float",
    "branches": "int",
    "curveRes": "int",
    "length": "float",
    "lengthV": "float",
    "curve": "float",
    "curveV": "float",
    "curveBack": "float",
    "downAngle": "float",
    "downAngleV": "float",
    "rotate": "float",
    "rotateV": "float",
    "splitAngle": "float",
    "splitAngleV": "float",
    "segSplits": "float",
    "attractUp": "float",
    "attractOut": "float",
    "taper": "float",
    "radiusTweak": "float",
}

EXPORT_FIELDS = ("per_seg", "leaf_aspect", "anchor_mode", "leaf_type_desc")

# 传输顺序固定 = proto 里的字段顺序，保证同一份配置永远生成同一条命令行。
TREE_ORDER = ["preset", "extra_set"] + list(SCALAR_FIELDS) + list(ARRAY_FIELDS)

TOP_LEVEL_FIELDS = ("version", "name", "description", "tree", "export")


class ConfigError(Exception):
    """配置写错（字段名/取值/数组长度）。消息里带行号，直接给人看。"""


# ---------------------------------------------------------------------------
# pbtxt 解析（只支持本 schema 用到的子集：标量 / 字符串 / 布尔 / 块 / 重复字段）
# ---------------------------------------------------------------------------
def _strip_comment(line):
    """去掉行尾注释。引号里的 '#' 不算注释。"""
    in_string = False
    escaped = False
    for i, ch in enumerate(line):
        if escaped:
            escaped = False
            continue
        if ch == "\\":
            escaped = True
            continue
        if ch == '"':
            in_string = not in_string
            continue
        if ch == "#" and not in_string:
            return line[:i]
    return line


def _parse_value(text, lineno):
    """把一个标量值解析成 str / int / float / bool。"""
    text = text.strip()
    if not text:
        raise ConfigError("第 %d 行：字段缺少值" % lineno)
    if text[0] == '"':
        if len(text) < 2 or text[-1] != '"':
            raise ConfigError("第 %d 行：字符串没有闭合（还支持 \\\" 转义）" % lineno)
        body = text[1:-1]
        return body.replace('\\"', '"').replace("\\\\", "\\")
    if text in ("true", "false"):
        return text == "true"
    try:
        return int(text)
    except ValueError:
        pass
    try:
        return float(text)
    except ValueError:
        return text          # 裸标识符（enum 字符串，例如 rotate）


def _merge(target, key, value, lineno, repeated):
    if repeated:
        target.setdefault(key, []).append(value)
        return
    if key in target:
        raise ConfigError("第 %d 行：字段 '%s' 只能出现一次（数组字段才可重复）" % (lineno, key))
    target[key] = value


def parse_config(text):
    """pbtxt 文本 -> dict。**保留「字段是否出现」**：未写的字段不在 dict 里。

    这一点是语义的一部分：未写 = 用 Sapling 预设自带值，写了 0 = 真的设 0。
    """
    root = {}
    stack = [(root, None)]
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = _strip_comment(raw).strip()
        if not line:
            continue
        if line == "}":
            if len(stack) == 1:
                raise ConfigError("第 %d 行：多余的 '}'" % lineno)
            stack.pop()
            continue
        if "}" in line:
            raise ConfigError("第 %d 行：'}' 必须单独占一行（不支持单行块）" % lineno)
        if "{" in line and not line.endswith("{"):
            raise ConfigError("第 %d 行：块必须写成单独一行 'name {'" % lineno)
        if line.endswith("{"):
            name = line[:-1].strip().rstrip(":").strip()
            if not name:
                raise ConfigError("第 %d 行：块缺少名字" % lineno)
            child = {}
            _merge(stack[-1][0], name, child, lineno, repeated=False)
            stack.append((child, name))
            continue
        if ":" not in line:
            raise ConfigError("第 %d 行：既不是 'key: value' 也不是 'key {'" % lineno)
        key, _, value_text = line.partition(":")
        key = key.strip()
        if not key:
            raise ConfigError("第 %d 行：缺少字段名" % lineno)
        parent = stack[-1][0]
        block_name = stack[-1][1]
        repeated = block_name == "tree" and (key in ARRAY_FIELDS or key == "extra_set")
        _merge(parent, key, _parse_value(value_text, lineno), lineno, repeated)
    if len(stack) != 1:
        raise ConfigError("有 %d 个块没有闭合（少了 '}'）" % (len(stack) - 1))
    return root


# ---------------------------------------------------------------------------
# 校验：字段名/类型/取值范围
# ---------------------------------------------------------------------------
def _require(cond, message):
    if not cond:
        raise ConfigError(message)


def _check_scalar(name, value, kind):
    if kind == "float":
        _require(isinstance(value, (int, float)) and not isinstance(value, bool),
                 "字段 '%s' 需要数字，实际是 %r" % (name, value))
    elif kind == "int":
        _require(isinstance(value, int) and not isinstance(value, bool),
                 "字段 '%s' 需要整数，实际是 %r" % (name, value))
    elif kind == "bool":
        _require(isinstance(value, bool), "字段 '%s' 需要 true/false，实际是 %r" % (name, value))
    elif kind == "rMode":
        _require(value in R_MODES, "字段 'rMode' 只能是 %s，实际是 %r" % ("/".join(R_MODES), value))
    elif kind in ("shape", "shapeS", "leafDist"):
        allowed = SHAPE_IDS if kind == "shape" else SHAPE_IDS_NO_CUSTOM
        _require(value in allowed,
                 "字段 '%s' 需要枚举 id %s，实际是 %r（8=Custom Shape 只有 shape 支持）"
                 % (name, list(allowed), value))


def validate_config(cfg):
    """校验并补默认值。返回规范化后的 dict。"""
    for key in cfg:
        _require(key in TOP_LEVEL_FIELDS,
                 "未知的顶层字段 '%s'（允许：%s）" % (key, ", ".join(TOP_LEVEL_FIELDS)))

    version = cfg.get("version")
    _require(isinstance(version, int) and not isinstance(version, bool) and version == 1,
             "version 必须显式写成 1（当前为 %r）" % (version,))
    _require(isinstance(cfg.get("name", ""), str), "name 需要字符串")
    _require(isinstance(cfg.get("description", ""), str), "description 需要字符串")

    tree = cfg.get("tree")
    _require(isinstance(tree, dict) and tree, "缺少 tree 块")
    for key in tree:
        _require(key in TREE_ORDER,
                 "tree 里未知字段 '%s'（拼错了？查 proto/generate.proto）" % key)
    preset = tree.get("preset")
    _require(isinstance(preset, str) and preset, "tree.preset 必填（Sapling 预设名）")

    for key, value in tree.items():
        if key in SCALAR_FIELDS:
            _check_scalar(key, value, SCALAR_FIELDS[key])
        elif key in ARRAY_FIELDS:
            _require(isinstance(value, list) and len(value) == ARRAY_LEN,
                     "字段 '%s' 需要 %d 个值（下标 = 枝级 0..3），实际 %d 个"
                     % (key, ARRAY_LEN, len(value) if isinstance(value, list) else 1))
            for item in value:
                _check_scalar(key, item, ARRAY_FIELDS[key])
        elif key == "extra_set":
            _require(isinstance(value, list) and all(isinstance(v, str) for v in value),
                     "extra_set 需要字符串列表（如 \"splitByLen=false\"）")
            for item in value:
                _require("=" in item, "extra_set 项必须是 k=v 形式，实际是 %r" % item)

    export = cfg.get("export")
    _require(isinstance(export, dict), "缺少 export 块")
    for key in export:
        _require(key in EXPORT_FIELDS,
                 "export 里未知字段 '%s'（允许：%s）" % (key, ", ".join(EXPORT_FIELDS)))
    _require(isinstance(export.get("per_seg"), int) and export["per_seg"] > 0,
             "export.per_seg 必须是正整数")
    _require(isinstance(export.get("leaf_aspect"), (int, float))
             and not isinstance(export["leaf_aspect"], bool)
             and export["leaf_aspect"] > 0,
             "export.leaf_aspect 必须 > 0（卡宽/高）")
    _require(export.get("anchor_mode") in ANCHOR_MODES,
             "export.anchor_mode 只能是 %s，实际是 %r"
             % ("/".join(ANCHOR_MODES), export.get("anchor_mode")))
    _require(isinstance(export.get("leaf_type_desc"), str) and export["leaf_type_desc"],
             "export.leaf_type_desc 必填（写进 shape.pbtxt 的自然语言描述）")
    return cfg


def load_config(path):
    """读 + 解析 + 校验。"""
    with open(path, encoding="utf-8") as handle:
        cfg = parse_config(handle.read())
    validate_config(cfg)
    return cfg


# ---------------------------------------------------------------------------
# 配置 -> 命令行（纯函数，可单测）
# ---------------------------------------------------------------------------
def _format_value(value, kind):
    if kind == "bool":
        return "true" if value else "false"
    if kind == "int":
        return str(int(value))
    if kind == "rMode":
        return str(value)          # 枚举字符串，原样给 Sapling
    return "%g" % float(value)


def build_overrides(tree):
    """tree 块 -> Sapling 的 `--set k=v` 列表（顺序固定，同配置生成同命令行）。"""
    overrides = []
    for key in TREE_ORDER:
        if key not in tree or key in ("preset", "extra_set"):
            continue
        if key in SCALAR_FIELDS:
            overrides.append("%s=%s" % (key, _format_value(tree[key], SCALAR_FIELDS[key])))
        else:
            kind = ARRAY_FIELDS[key]
            overrides.append("%s=%s" % (key, ",".join(_format_value(v, kind) for v in tree[key])))
    # 逃生口放最后：同名 key 覆盖上面的强类型字段（与 proto 注释一致）。
    overrides.extend(tree.get("extra_set", []))
    return overrides


def build_command(cfg, out_path, blender="blender", exporter=EXPORT_SCRIPT):
    """配置 -> 完整的 blender 命令行（argv 列表，不经过 shell）。"""
    tree = cfg["tree"]
    export = cfg["export"]
    cmd = [blender, "-b", "--python", exporter, "--",
           tree["preset"],
           "--out", out_path,
           "--per-seg", str(export["per_seg"]),
           "--leaf-aspect", "%g" % float(export["leaf_aspect"]),
           "--anchor-mode", export["anchor_mode"],
           "--leaf-type-desc", export["leaf_type_desc"]]
    for item in build_overrides(tree):
        cmd.extend(["--set", item])
    return cmd


# ---------------------------------------------------------------------------
# 产物检查（纯函数，可单测）
# ---------------------------------------------------------------------------
BRANCH_BLOCK_RE = re.compile(r"^branches \{", re.MULTILINE)
CARD_BLOCK_RE = re.compile(r"^cards \{", re.MULTILINE)
ID_RE = re.compile(r"^\s*id: (-?\d+)$", re.MULTILINE)
PARENT_RE = re.compile(r"^\s*parent: (-?\d+)$", re.MULTILINE)


def check_shape_text(text):
    """检查导出的 shape.pbtxt 是否**能被 Validate 接受**。

    Validate 的硬约束里有两条最容易在导出侧被破坏，这里先兜住，免得把坏文件交给下游：
      * 恰好一个根枝（parent = -1）
      * 父枝必须出现在自身之前（拓扑序）
    其它约束（半径 > 0、卡片 width > 0 …）交给 C++ 的 Validate，不在这里重复实现。
    """
    branch_blocks = BRANCH_BLOCK_RE.split(text)[1:]
    ids = []
    parents = []
    for block in branch_blocks:
        head = block.split("samples {", 1)[0]
        id_match = ID_RE.search(head)
        parent_match = PARENT_RE.search(head)
        if id_match is None or parent_match is None:
            raise ConfigError("产物里有 branches 块缺少 id/parent")
        ids.append(int(id_match.group(1)))
        parents.append(int(parent_match.group(1)))

    roots = [b for b, p in zip(ids, parents) if p == -1]
    position = {}
    for index, bid in enumerate(ids):
        position.setdefault(bid, index)
    bad = [(b, p) for index, (b, p) in enumerate(zip(ids, parents))
           if p != -1 and position.get(p, 1 << 30) >= index]
    return {
        "branches": len(branch_blocks),
        "cards": len(CARD_BLOCK_RE.findall(text)),
        "roots": len(roots),
        "topo_violations": bad[:5],
        "topo_violation_count": len(bad),
    }


def check_shape_file(path):
    if not os.path.exists(path):
        raise ConfigError("blender 返回成功，但没有写出产物 %s" % path)
    if os.path.getsize(path) == 0:
        raise ConfigError("产物 %s 是空文件" % path)
    with open(path, encoding="utf-8") as handle:
        stats = check_shape_text(handle.read())
    stats["bytes"] = os.path.getsize(path)
    return stats


# ---------------------------------------------------------------------------
# 唯一不可单测的部分：真的起一个 Blender
# ---------------------------------------------------------------------------
def run_blender(cmd):
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True)
    except FileNotFoundError:
        raise ConfigError(
            "找不到 blender 可执行文件 %r。Blender 由镜像提供（开发期工具，不在本仓库里）；"
            "用 --blender 指到实际路径。" % cmd[0])
    if proc.returncode != 0:
        tail = "\n".join((proc.stdout + proc.stderr).strip().splitlines()[-20:])
        raise ConfigError("blender 退出码 %d，最后 20 行输出：\n%s" % (proc.returncode, tail))
    return proc


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="从生成配置 pbtxt 调 Blender/Sapling 出 shape.pbtxt")
    parser.add_argument("config", help="生成配置（proto/generate.proto 的文本格式）")
    parser.add_argument("--out", required=True, help="产物 shape.pbtxt 路径")
    parser.add_argument("--blender", default="blender", help="blender 可执行文件（默认 PATH 里的）")
    parser.add_argument("--dry-run", action="store_true",
                        help="只打印将要执行的 blender 命令，不真跑")
    args = parser.parse_args(argv)

    try:
        cfg = load_config(args.config)
    except ConfigError as error:
        print("配置错误: %s" % error, file=sys.stderr)
        return 2

    name = cfg.get("name") or os.path.basename(args.config)
    print("[gen] %s  (preset=%s)" % (name, cfg["tree"]["preset"]))
    if cfg.get("description"):
        print("[gen] %s" % cfg["description"])

    cmd = build_command(cfg, args.out, blender=args.blender)
    if args.dry_run:
        print("[gen] dry-run，未执行：\n%s" % shlex.join(cmd))
        return 0

    started = time.monotonic()
    print("[gen] 调用 Blender（%d 个参数覆盖）…" % len(build_overrides(cfg["tree"])))
    try:
        run_blender(cmd)
        stats = check_shape_file(args.out)
    except ConfigError as error:
        print("生成失败: %s" % error, file=sys.stderr)
        return 1

    if stats["roots"] != 1 or stats["topo_violation_count"]:
        print("生成失败: 产物的拓扑序不合法（根数 %d，违规 %d 处，例如 %s）。\n"
              "这是 export_vegetation_shape.py 的已知缺陷：BFS 重排的「兜底」会把不可达的\n"
              "spline 直接追加到末尾，导致子枝排在父枝之后。先调小 baseSplits / branches 绕开。"
              % (stats["roots"], stats["topo_violation_count"], stats["topo_violations"]),
              file=sys.stderr)
        return 1

    print("[gen] OK  %d 枝 / %d 卡 / %.1f MB / %.1f s → %s"
          % (stats["branches"], stats["cards"], stats["bytes"] / 1e6,
             time.monotonic() - started, args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
