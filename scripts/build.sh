#!/bin/bash
# ============================================================================
# ESP32-S3 宏键盘 / LVGL 4.3" (HX8369 + GT911)
#
# 编译脚本：把仓库同步到 WSL 原生文件系统后调用 idf.py build。
# 在 /mnt/c 上直接编译会非常慢，所以先用 rsync 同步。
#
# 用法（Windows 侧）:
#   wsl.exe -e bash /mnt/c/ws/espView/esp32KeyBoard/scripts/build.sh
# ============================================================================
set -o pipefail

# 仓库根目录（本脚本位于 <repo>/scripts/build.sh）
REPO_SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# WSL 原生构建目录
BUILD_DIR="${IDF_BUILD_DIR:-$HOME/ws/esp32KeyBoard-build}"

# ---- 1. 加载 ESP-IDF 环境 --------------------------------------------------
# 优先使用调用者已设置的环境，其次使用本机约定的 env.sh
if ! command -v idf.py >/dev/null 2>&1; then
    if [ -f "$HOME/ws/env.sh" ]; then
        # shellcheck disable=SC1091
        source "$HOME/ws/env.sh"
    else
        echo "错误: 找不到 idf.py，请先 source 你的 ESP-IDF export.sh" >&2
        exit 1
    fi
fi

# ---- 2. 同步源码到 WSL 原生盘 ----------------------------------------------
mkdir -p "$BUILD_DIR"
rsync -a --delete \
      --exclude 'build/' \
      --exclude 'sdkconfig.old' \
      "$REPO_SRC"/ "$BUILD_DIR"/

# ---- 3. 编译 ---------------------------------------------------------------
cd "$BUILD_DIR" || exit 1
LOG="${BUILD_LOG:-$BUILD_DIR/build.log}"
idf.py build > "$LOG" 2>&1
RC=$?

echo "=============================================================="
echo "IDF    : $(idf.py --version 2>/dev/null)"
if [ $RC -eq 0 ]; then
    echo "RESULT : BUILD OK"
    grep -E "binary size|Project build complete" "$LOG" | tail -4
else
    echo "RESULT : BUILD FAILED (exit $RC)"
    grep -E "error:|FAILED" "$LOG" | head -20
fi
echo "log    : $LOG"
echo "=============================================================="
exit $RC
