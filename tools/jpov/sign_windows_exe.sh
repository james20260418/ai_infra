#!/usr/bin/env bash
# =============================================================================
# sign_windows_exe.sh — 给交叉编译出来的 .exe 做本地代码签名
#
# 为什么需要：Windows 上「未签名 + 有网络来源标记（MOTW）」的 exe 会触发
# SmartScreen（"Windows 已保护你的电脑 / 未查到此应用的发布信息"）并要求 Defender
# 扫描十几秒。签名后本机认可发布者，就不再弹这个框。
#
# ⚠️ 这里用的是**自签名**证书（10 年），只在本机可信：
#   第一次生成后，把 output/signing/jpov_codesign.crt 装进
#   「受信任的根证书颁发机构」+「受信任的发布者」（双击 → 安装证书 → 本地计算机），
#   之后这台机器上所有用同一证书签的 exe 都不再报未知发布者。
#
# ⚠️ 私钥不进 git：证书全部放在 output/signing/（output/ 已被 .gitignore 忽略）。
#
# 用法：
#   ./tools/jpov/sign_windows_exe.sh output/jpov_skylight_viewer/jpov_skylight_viewer.exe
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
SIGN_DIR="$PROJECT_DIR/output/signing"
CERT="$SIGN_DIR/jpov_codesign.crt"
KEY="$SIGN_DIR/jpov_codesign.key"
P12="$SIGN_DIR/jpov_codesign.pfx"
P12_PASS="jpov-local-sign"

EXE="${1:-}"
if [[ -z "$EXE" ]]; then
    echo "用法: $0 <path-to-exe>" >&2
    exit 2
fi
if [[ ! -f "$EXE" ]]; then
    echo "找不到 exe: $EXE" >&2
    exit 2
fi
if ! command -v osslsigncode >/dev/null 2>&1; then
    echo "缺少 osslsigncode（apt-get install -y osslsigncode），跳过签名" >&2
    exit 0
fi

mkdir -p "$SIGN_DIR"

if [[ ! -f "$CERT" || ! -f "$P12" ]]; then
    echo "==> 生成本地自签名代码签名证书（10 年，仅本机可信）"
    openssl req -x509 -newkey rsa:3072 -nodes -days 3650 \
        -keyout "$KEY" -out "$CERT" \
        -subj "/CN=JPOV Local Dev/O=JPOV/OU=Local Build" \
        -addext "extendedKeyUsage=codeSigning" \
        -addext "keyUsage=critical,digitalSignature" >/dev/null 2>&1
    openssl pkcs12 -export -out "$P12" -inkey "$KEY" -in "$CERT" \
        -passout "pass:$P12_PASS" >/dev/null 2>&1
    echo "    证书: $CERT"
    echo "    私钥: $KEY（不要提交到 git）"
fi

echo "==> 签名 $(basename "$EXE")"
TMP_SIGNED="$EXE.signed"
osslsigncode sign \
    -pkcs12 "$P12" -pass "$P12_PASS" \
    -n "JPOV" -i "https://localhost/jpov" \
    -h sha256 -in "$EXE" -out "$TMP_SIGNED" >/dev/null
mv "$TMP_SIGNED" "$EXE"

echo "==> 校验签名"
osslsigncode verify -in "$EXE" | sed -n '1,12p'

echo ""
echo "============================================"
echo "  签名完成"
echo "============================================"
echo "首次使用时，把这张证书装进本机信任（只需一次）："
echo "  $CERT"
echo "  双击 → 安装证书 → 本地计算机 → 放入下列存储 →「受信任的根证书颁发机构」"
echo "  再装一次到「受信任的发布者」（同一张 .crt 可以装两处）"
echo ""
echo "另外：如果 exe 是从网上下载/复制来的，文件会带「网络来源」标记（MOTW），"
echo "即使签了名也可能被 SmartScreen 拦。右键属性勾「解除锁定」，或 PowerShell:"
echo "  Unblock-File -Path <exe>"
