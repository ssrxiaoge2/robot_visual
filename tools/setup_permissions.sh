#!/bin/bash
set -e

echo "=== 补光灯权限配置 ==="

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BIN="$SCRIPT_DIR/fill_light"

echo "[1/2] 为 fill_light 添加执行权限..."
chmod +x "$BIN"
echo "  OK"

echo "[2/2] 授予 I/O 端口访问权限 (CAP_SYS_RAWIO)..."
sudo setcap cap_sys_rawio+ep "$BIN"
echo "  OK"

echo ""
echo "=== 验证 ==="
getcap "$BIN"
echo ""
echo "=== 配置完成 ==="
