"""ESP32-S3 串口监视器（USB-Serial/JTAG）。

用法:
    python monitor.py [PORT] [SECONDS] [reset|noreset]

通过 DTR/RTS 触发芯片复位后采集日志，过滤 ANSI 颜色码，
并按关键事件（启动、断言、panic、触摸上报、错误行）做统计。
"""
import os
import re
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM6"
SECONDS = int(sys.argv[2]) if len(sys.argv) > 2 else 15
DO_RESET = (len(sys.argv) <= 3) or (sys.argv[3].lower() != "noreset")

# 日志写到脚本所在目录的上一级
LOG = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "serial_log.txt")


def main():
    ser = serial.Serial(PORT, 115200, timeout=0.2)
    if DO_RESET:
        # ESP32-S3 USB-Serial/JTAG 复位序列
        ser.setDTR(False)
        ser.setRTS(True)
        time.sleep(0.1)
        ser.setRTS(False)

    buf = bytearray()
    start = time.time()
    while time.time() - start < SECONDS:
        data = ser.read(4096)
        if data:
            buf.extend(data)
    ser.close()

    text = re.sub(r"\x1b\[[0-9;]*m", "", buf.decode("utf-8", errors="replace"))
    with open(LOG, "w", encoding="utf-8") as fh:
        fh.write(text)

    lines = [ln.rstrip() for ln in text.splitlines() if ln.strip()]
    boots = sum(1 for ln in lines if "ESP-ROM:esp32s3" in ln)
    asserts = sum(1 for ln in lines if "assert failed" in ln)
    panics = sum(1 for ln in lines if "Guru Meditation" in ln or "abort()" in ln)
    errors = [ln for ln in lines if ln.startswith("E (")]
    touches = sum(1 for ln in lines if "GT911: X=" in ln)

    print("=" * 62)
    print(f"  采集 {len(buf)} 字节 / {len(lines)} 行  ->  {LOG}")
    print("=" * 62)
    print(f"  启动次数      : {boots}")
    print(f"  断言失败      : {asserts}")
    print(f"  panic         : {panics}")
    print(f"  触摸上报次数  : {touches}")
    print(f"  错误行(E)     : {len(errors)}")
    if errors:
        print("  --- 错误详情（前 10 条）---")
        for ln in errors[:10]:
            print("   ", ln)

    print("\n--- 启动 / 外设初始化 ---")
    for ln in lines:
        if re.search(r"ESP-IDF|Display buffer|lcd_panel|backlight|lvgl_helpers|"
                     r"Starting I2C|Initialised port|Product ID|Vendor ID|Resolution|"
                     r"BLE HID|NimBLE host|host connected|battery", ln):
            print(" ", ln)
    print("=" * 62)


if __name__ == "__main__":
    main()
