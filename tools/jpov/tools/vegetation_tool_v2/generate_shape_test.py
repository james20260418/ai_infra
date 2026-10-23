#!/usr/bin/env python3
"""generate_shape.py 的单测（纯逻辑，不需要 Blender、不联网、秒级）。

    python3 tools/jpov/tools/vegetation_tool_v2/generate_shape_test.py

「调 Blender」那一步没法单测，所以 generate_shape.py 把逻辑都挤到了纯函数里，
这里覆盖的就是那些纯函数：
  parse_config / validate_config / build_overrides / build_command / check_shape_text
外加：**仓库里那三个 configs/*.pbtxt 必须始终能通过校验**（防止改 schema 时漏改配置）。

每条断言都带负向验证（故意改坏 → 必须报错），否则测试会变成"永远绿"的样子货。
"""

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import generate_shape as G  # noqa: E402  （需要上面的 sys.path）

HERE = os.path.dirname(os.path.abspath(__file__))
CONFIG_DIR = os.path.join(HERE, "configs")

_checks = 0
_failures = []


def check(condition, message):
    global _checks
    _checks += 1
    if not condition:
        _failures.append(message)


def expect_error(fn, fragment, message):
    """断言 fn() 抛 ConfigError 且消息里含 fragment（负向验证用）。"""
    global _checks
    _checks += 1
    try:
        fn()
    except G.ConfigError as error:
        if fragment not in str(error):
            _failures.append("%s：报错信息里没有 %r，实际是 %r" % (message, fragment, str(error)))
        return
    _failures.append("%s：本该报错，却通过了" % message)


MINIMAL = """
version: 1
name: "t"
description: "说明"
tree {
  preset: "small_pine"
  scale: 9
  seed: 2
  leaves: 40
}
export {
  per_seg: 8
  leaf_aspect: 0.45
  anchor_mode: "bottom_left"
  leaf_type_desc: "叶"
}
"""


def test_parse_basics():
    cfg = G.parse_config(MINIMAL)
    check(cfg["version"] == 1, "version 解析")
    check(cfg["tree"]["preset"] == "small_pine", "preset 解析")
    check(cfg["tree"]["scale"] == 9.0, "scale 解析成 float")
    check(cfg["tree"]["seed"] == 2, "seed 解析成 int")
    check(isinstance(cfg["export"]["per_seg"], int), "per_seg 解析成 int")
    # 语义要害：没写的字段**不能**出现在 dict 里（未写 ≠ 0）
    check("levels" not in cfg["tree"], "未写字段不应出现在 dict 里")
    check("baseSize" not in cfg["tree"], "未写字段不应出现在 dict 里")


def test_parse_arrays_and_comments():
    text = """
version: 1
name: "a#b"        # 引号里的 # 不是注释
tree {
  preset: "small_maple"
  downAngle: 90
  downAngle: 72
  downAngle: 58
  downAngle: 48
}
export {
  per_seg: 6
  leaf_aspect: 0.7
  anchor_mode: "bottom_left"
  leaf_type_desc: "x"
}
"""
    cfg = G.parse_config(text)
    check(cfg["name"] == "a#b", "字符串里的 '#' 不应被当注释")
    check(cfg["tree"]["downAngle"] == [90.0, 72.0, 58.0, 48.0], "重复字段应聚成数组")
    check(cfg["export"]["per_seg"] == 6, "同一行的块内字段应能解析")


def test_parse_errors():
    expect_error(lambda: G.parse_config("version: 1\ntree {\n  preset: \"x\"\n"),
                 "没有闭合", "缺少 '}' 应报错")
    expect_error(lambda: G.parse_config("version: 1\n}\n"),
                 "多余的", "多余的 '}' 应报错")
    expect_error(lambda: G.parse_config('version: 1\nname: "a\n'),
                 "字符串没有闭合", "未闭合字符串应报错")
    expect_error(lambda: G.parse_config("version: 1\nname: a\nname: b\n"),
                 "只能出现一次", "重复的标量字段应报错")
    expect_error(lambda: G.parse_config("version: 1\nexport { per_seg: 6 }\n"),
                 "单独占一行", "单行块应报错（不自作聪明拼接）")


def test_validate_errors():
    base = G.parse_config(MINIMAL)

    def mutate(path, value):
        cfg = {k: (dict(v) if isinstance(v, dict) else v) for k, v in base.items()}
        cfg["tree"] = dict(base["tree"])
        cfg["export"] = dict(base["export"])
        target = cfg if path[0] == "root" else cfg[path[0]]
        key = path[-1]
        if value is None:
            target.pop(key, None)
        else:
            target[key] = value
        return lambda: G.validate_config(cfg)

    expect_error(mutate(("root", "version"), 2), "version", "version 必须是 1")
    expect_error(mutate(("tree", "preset"), None), "preset", "缺 preset 应报错")
    expect_error(mutate(("tree", "scale"), "big"), "需要数字", "scale 类型错应报错")
    expect_error(mutate(("tree", "seed"), 1.5), "需要整数", "seed 用小数应报错")
    expect_error(mutate(("tree", "splitByLen"), 1), "true/false", "bool 字段收到 1 应报错")
    expect_error(mutate(("tree", "typoField"), 1), "未知字段", "拼错字段名应报错")
    expect_error(mutate(("tree", "downAngle"), [90.0, 72.0]), "4 个值", "数组长度错应报错")
    expect_error(mutate(("tree", "shape"), 9), "枚举", "shape=9 不存在应报错")
    expect_error(mutate(("tree", "rMode"), "spiral"), "rMode", "非法 rMode 应报错")
    expect_error(mutate(("export", "anchor_mode"), "center"), "anchor_mode", "非法 anchor_mode 应报错")
    expect_error(mutate(("export", "per_seg"), 0), "正整数", "per_seg=0 应报错")
    expect_error(mutate(("export", "leaf_aspect"), 0), "必须 > 0", "leaf_aspect=0 应报错")
    expect_error(mutate(("export", "leaf_type_desc"), ""), "leaf_type_desc", "空描述应报错")
    expect_error(mutate(("root", "unknownTop"), 1), "顶层字段", "未知顶层字段应报错")

    # 正向对照：没改动的那份必须过
    G.validate_config(base)


def test_build_overrides():
    cfg = G.parse_config("""
version: 1
name: "x"
tree {
  preset: "small_maple"
  scale: 9
  seed: 7
  closeTip: false
  rMode: "random"
  length: 0.6
  length: 0.8
  length: 0.6
  length: 0.4
  branches: 0
  branches: 30
  branches: 13
  branches: 7
}
export {
  per_seg: 8
  leaf_aspect: 0.72
  anchor_mode: "bottom_left"
  leaf_type_desc: "x"
}
""")
    G.validate_config(cfg)
    overrides = G.build_overrides(cfg["tree"])
    check("scale=9" in overrides, "float 用 %%g 输出（9 而不是 9.0）：%r" % (overrides,))
    check("seed=7" in overrides, "int 原样输出")
    check("closeTip=false" in overrides, "bool 输出 true/false（export 脚本按这个解析）")
    check('rMode=random' in overrides, "enum 字符串原样输出")
    check("length=0.6,0.8,0.6,0.4" in overrides, "数组用逗号拼")
    check("branches=0,30,13,7" in overrides, "int 数组用逗号拼")
    check("preset" not in "".join(overrides), "preset 不应混进 --set（它走位置参数）")
    # 顺序必须稳定：同一份配置两次生成的命令行要逐字相同
    check(overrides == G.build_overrides(cfg["tree"]), "overrides 顺序应稳定")

    # extra_set 逃生口：追加在最后，同名覆盖强类型字段
    cfg2 = G.parse_config("""
version: 1
name: "y"
tree {
  preset: "small_pine"
  scale: 9
  extra_set: "scale=11"
  extra_set: "splitByLen=false"
}
export {
  per_seg: 8
  leaf_aspect: 0.5
  anchor_mode: "bottom_left"
  leaf_type_desc: "x"
}
""")
    G.validate_config(cfg2)
    overrides2 = G.build_overrides(cfg2["tree"])
    check(overrides2[-2:] == ["scale=11", "splitByLen=false"],
          "extra_set 必须排在最后：%r" % (overrides2[-2:],))
    check(overrides2.index("scale=9") < overrides2.index("scale=11"),
          "同名 key 后者应能覆盖前者")


def test_build_command():
    cfg = G.load_config(os.path.join(CONFIG_DIR, "pine.pbtxt"))
    cmd = G.build_command(cfg, "out/pine.pbtxt", blender="/opt/blender")
    check(cmd[0] == "/opt/blender", "blender 路径可覆盖（镜像里可能不在 PATH）")
    check("--python" in cmd, "必须用 --python 加载导出脚本")
    check(cmd[cmd.index("--out") + 1] == "out/pine.pbtxt", "--out 透传")
    check(cmd[cmd.index("--per-seg") + 1] == "8", "--per-seg 透传")
    check(cmd[cmd.index("--anchor-mode") + 1] == "bottom_left", "--anchor-mode 透传")
    # Blender 的约定：脚本自己的参数必须在 "--" 之后
    check(cmd.index("--") < cmd.index("--out"), "脚本参数必须在 '--' 之后")
    check(cmd.count("--set") == len(G.build_overrides(cfg["tree"])),
          "--set 数量应与 overrides 一致")


def test_check_shape_text():
    good = """version: 1
description: "x"
branches {
  id: 0
  parent: -1
  samples {
    position { x: 0 y: 0 z: 0 }
    radius: 0.1
  }
}
branches {
  id: 1
  parent: 0
  samples {
    position { x: 0 y: 1 z: 0 }
    radius: 0.05
  }
}
cards {
  anchor { x: 0 y: 1 z: 0 }
  height: 0.2
  width: 0.1
  branch: 1
  s: 0.5
}
"""
    stats = G.check_shape_text(good)
    check(stats["branches"] == 2 and stats["cards"] == 1, "统计枝/卡数")
    check(stats["roots"] == 1, "恰好一个根")
    check(stats["topo_violation_count"] == 0, "合法文件不应报拓扑违规")

    # 子枝排在父枝前面（就是导出脚本兜底会踩的坑）
    broken = """version: 1
branches {
  id: 1
  parent: 0
  samples {
    position { x: 0 y: 1 z: 0 }
    radius: 0.05
  }
}
branches {
  id: 0
  parent: -1
  samples {
    position { x: 0 y: 0 z: 0 }
    radius: 0.1
  }
}
"""
    stats_broken = G.check_shape_text(broken)
    check(stats_broken["topo_violation_count"] == 1,
          "父枝在子枝之后必须被判定为违规：%r" % (stats_broken,))

    two_roots = broken.replace("parent: 0", "parent: -1")
    check(G.check_shape_text(two_roots)["roots"] == 2, "两个根必须能数出来")

    # 产物不存在 / 空文件：不能让 traceback 滤到用户脸上
    missing = os.path.join(tempfile.gettempdir(), "__veg_no_such_shape__.pbtxt")
    if os.path.exists(missing):
        os.remove(missing)
    expect_error(lambda: G.check_shape_file(missing), "没有写出产物", "产物不存在应报错")


def test_checked_in_configs():
    """仓库里的配置必须始终自洽（改 schema 忘了改配置，这里会红）。"""
    names = sorted(f for f in os.listdir(CONFIG_DIR) if f.endswith(".pbtxt"))
    check(names == ["fir.pbtxt", "oak.pbtxt", "pine.pbtxt"],
          "configs/ 里应当正好是这三份：%r" % (names,))
    for name in names:
        cfg = G.load_config(os.path.join(CONFIG_DIR, name))
        check(cfg["name"] == name[:-len(".pbtxt")], "%s 的 name 应与文件名一致" % name)
        overrides = G.build_overrides(cfg["tree"])
        check(len(overrides) >= 3, "%s 的覆盖参数太少，像是没读进来" % name)
        cmd = G.build_command(cfg, "out.pbtxt")
        check("--set" in cmd, "%s 的命令行里应有 --set" % name)


def main():
    for fn in (test_parse_basics, test_parse_arrays_and_comments, test_parse_errors,
               test_validate_errors, test_build_overrides, test_build_command,
               test_check_shape_text, test_checked_in_configs):
        before = len(_failures)
        fn()
        print("%-34s %s" % (fn.__name__, "ok" if len(_failures) == before else "FAIL"))

    print("\n%d 条断言，%d 条失败" % (_checks, len(_failures)))
    for failure in _failures:
        print("  - %s" % failure)
    return 1 if _failures else 0


if __name__ == "__main__":
    sys.exit(main())
