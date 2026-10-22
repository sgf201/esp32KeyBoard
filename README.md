# ESP32-S3 宏键盘 + 副屏

把一块 **4.3 寸 800×480 电容触摸屏**做成 PC / 安卓的**蓝牙无线键盘 + 触摸板**，
同时保留 LVGL 界面做状态显示。

- **主控**：ESP32-S3（QFN56 v0.2，双核 240MHz，8MB PSRAM，16MB Flash）
- **屏幕**：4.3" 800×480，显示控制器 **HX8369**（Intel 8080 并口，8bit）
- **触摸**：**GT911** 电容触摸（I2C，SDA=39 / SCL=38，400kHz，硬件支持 5 点）
- **连接**：BLE HID（HOGP），键盘 + 鼠标复合设备，无需 USB 线
- **框架**：ESP-IDF v5.5.2 + LVGL（v8 API 树）

---

## 项目状态

| 阶段 | 内容 | 状态 |
|------|------|------|
| 1 | GT911 五点触控支持 | ✅ 已验证（实测 4 点同时） |
| 2 | BLE HID 键盘 + 鼠标通路 | ✅ 已验证（Windows 可配对收发） |
| 3 | 虚拟键盘 UI + 快捷键 | ⬜ 待做 |
| 4 | 触摸板 + 模式切换 | ⬜ 待做 |

---

## 快速开始

### 编译

```bash
bash scripts/build.sh
```

脚本会自动 `source` ESP-IDF 环境（默认找 `~/ws/env.sh`）并执行 `idf.py build`。

### 烧写

开发板通过 ESP32-S3 原生 **USB-Serial/JTAG** 接入，Windows 上枚举为 `COM6`。
WSL2 默认不接管 USB 设备，所以烧写在 Windows 侧用 esptool 完成：

```powershell
.\scripts\flash.ps1 -Port COM6
```

### 看日志

```powershell
.\scripts\monitor.ps1 -Port COM6 -Seconds 20
```

会复位板子、采集指定秒数，并统计启动次数 / 断言 / panic / 触摸上报 / 错误行。

---

## 硬件连接

| 功能 | 引脚 |
|------|------|
| LCD 数据 D0–D7 | 46, 3, 8, 18, 17, 16, 15, 7 |
| LCD PCLK / CS / DC / RST | 10 / 12 / 11 / 9 |
| 背光 | 6（高电平点亮） |
| 触摸 I2C SDA / SCL | 39 / 38 |
| USB | GPIO19 (D-) / GPIO20 (D+) |

面板时序：i80 8bit @ 20MHz。单帧 RGB565 = 750KB，总线理论上限约 **26 fps**。

---

## 代码结构

```
.
├── main/
│   ├── main.c            # 应用入口：LVGL 初始化、多点观察、HID 监督
│   ├── ble_hid.c/.h      # BLE HID 键盘 + 鼠标（复合报告）
│   └── esp_hid_gap.c/.h  # BLE GAP 辅助（源自 ESP-IDF 示例，已改造）
├── components/
│   ├── lvgl/             # LVGL（已裁剪，移除文档/示例/未启用 demo）
│   ├── lvgl_esp32_drivers/  # 显示与触摸驱动（已适配 IDF 5.x）
│   └── esp32s3_soc/      # 厂商预编译库，提供 GT911_RST()
├── scripts/
│   ├── build.sh          # WSL 编译
│   ├── flash.ps1         # Windows 烧写
│   ├── monitor.ps1/.py   # 串口日志采集与统计
│   ├── clear_bt_cache.ps1# 清理 Windows 蓝牙配对缓存（需管理员）
│   └── fix_bom.ps1       # 给 .ps1 补 UTF-8 BOM
├── partitions_singleapp_large.csv
└── sdkconfig
```

---

## 关键技术要点

### GT911 五点触控

驱动原先直接丢弃多点：

```c
if (touch_pnt_cnt != 1) {    // ignore no touch & multi touch
    ... return false;
}
```

现在改为 `gt911_read_points()`：一次 I2C 读 40 字节，连续取回全部 5 个点
（每点 8 字节：track id、X、Y、size、保留）。

**重要**：GT911 的状态寄存器是**读后清零**的。如果两个任务都去轮询它，
会互相偷走采样，表现为触摸丢帧。因此设计上：

- `gt911_read()`（LVGL 输入回调）是**唯一**允许访问 I2C 的读取者
- 其他模块通过 `gt911_get_last_sample()` 读快照，不碰 I2C

### BLE HID

ESP32-S3 **只有 BLE，没有经典蓝牙**（`SOC_BT_CLASSIC_SUPPORTED` 不存在），
所以走 HOGP（HID over GATT）而非普通蓝牙键盘的 BR/EDR。

键盘与鼠标放在**同一个报告映射**里，用不同 report ID 区分：

- Report ID 1：键盘（modifiers + 6 键位，标准 boot 布局）
- Report ID 2：鼠标（3 按键 + dx/dy/wheel）

对外 API 见 `main/ble_hid.h`：

```c
ble_hid_key_tap(BLE_HID_KEY_A, 0);                       // 打字母
ble_hid_key_combo(BLE_HID_MOD_LCTRL, BLE_HID_KEY_C);     // Ctrl+C
ble_hid_key_combo(BLE_HID_MOD_LCTRL | BLE_HID_MOD_LSHIFT,
                  BLE_HID_KEY_ESC);                      // Ctrl+Shift+Esc
ble_hid_mouse_move(10, 0, 0);                            // 相对移动
ble_hid_mouse_click(BLE_HID_MOUSE_LEFT);
ble_hid_mouse_wheel(3);                                  // 滚轮
ble_hid_set_battery(100);                                // 上报电量
```

---

## 踩过的坑（重要）

### 1. IDF 5.x API 变更导致编译失败

出厂工程按 IDF 4.x 写的，升级后需修改（均已加版本判断，仍兼容 4.x）：

| 文件 | 问题 | 修复 |
|------|------|------|
| `esp_lcd_backlight.c` | `ledc_timer_config_t.bit_num` 改名 | 改用 `duty_resolution` |
| `esp_lcd_backlight.c` | `gpio_matrix_out` / `gpio_pad_select_gpio` 移除 | 改用 `esp_rom_gpio_*` |
| `hx8369.c` | `esp_lcd_panel_t.disp_off` 改名 | 改用 `disp_on_off` |
| `i2c_manager.c` | `defined(I2C_NUM_0)` 恒假（枚举非宏） | 显式启用端口 0 |
| `i2c_manager.c` | `portTICK_RATE_MS` 改名 | 兼容宏到 `portTICK_PERIOD_MS` |

### 2. HX8369 初始化崩溃：i80 格式缓冲区只有 32 字节

IDF 5.x 的 i80 驱动有固定 `LCD_I80_IO_FORMAT_BUF_SIZE = 32` 的缓冲区，
而 HX8369 的 gamma 表 34 字节、DGC 表 127 字节、色彩表 192 字节，
一次性发送会触发断言并无限重启。

修复：`hx8369_tx_param()` 把超长参数拆成多笔发送（首笔带命令，
后续用 `lcd_cmd = -1` 走纯数据阶段）。

### 3. 加入蓝牙后 LVGL 绘制崩溃

LVGL 默认用内置分配器，在 `.bss` 里占 128KB 内部 RAM，
把蓝牙栈和 DMA 缓冲挤没了，`heap_caps_malloc` 返回 NULL，
LVGL 写空指针直接 `StoreProhibited`。

修复：`lv_conf.h` 里把 `LV_MALLOC/LV_REALLOC/LV_FREE` 指向标准
`malloc`（ESP-IDF 下走 PSRAM），并给绘制缓冲分配加了失败检查。

### 4. NimBLE 主机任务从未启动

`esp_hid_gap.c` 的 `init_low_level()` 只调用了 `esp_nimble_init()`，
没有 `nimble_port_freertos_init()`，导致主机栈空转、设备从不广播。

### 5. Windows 配对失败 / 反复掉线

多个问题叠加，逐个解决：

| 现象 | 原因 | 修复 |
|------|------|------|
| "请重新尝试连接" | `DISP_ONLY` + `sm_mitm=1` 矛盾 | 改 `NO_IO` + Just Works |
| 连上后 90ms 断开 | 没人发起加密，HID 需要加密链路 | GAP connect 事件里 `ble_gap_security_initiate()` |
| 重启后掉线 | 绑定密钥没落盘 | `CONFIG_BT_NIMBLE_NVS_PERSIST=y` |
| 广播 180 秒后消失 | 示例的广播有超时且不重启 | 保活任务检测 `ble_gap_adv_active()` |

**最隐蔽的一个**：反复用 `-Erase` 烧写会清掉设备端 NVS 里的绑定密钥，
但 Windows 仍缓存旧密钥，于是双方密钥不匹配，表现为"能连上但一直掉线"。
这在设置界面"删除设备"里**清不掉**，必须删注册表项：

```powershell
# 管理员 PowerShell
.\scripts\clear_bt_cache.ps1 -Mac cc8da2edb38a
Restart-Service bthserv -Force
```

> **教训**：调试 BLE 时不要随便 `-Erase`，否则每次都要重新配对。

### 6. PowerShell 脚本中文乱码

Windows PowerShell 5.1 读无 BOM 的 UTF-8 文件会按系统代码页解析，
中文会让语法解析失败（如报"字符串缺少终止符"）。

修复：编辑 `.ps1` 后跑一次 `scripts/fix_bom.ps1` 补 BOM。

---

## 已知限制

- **无法播放视频**：ESP32-S3 没有 JPEG / H.264 硬件解码器（`soc_caps.h`
  里无 `SOC_JPEG_*` / `SOC_H264_*`），软件解码 800×480 只有个位数帧率；
  且 i80 总线 20MB/s 也不足以支撑满帧刷新。视频场景请换 ESP32-P4。
- **经典蓝牙不支持**：只能 BLE HID，配对需在系统蓝牙设置里操作。
- **USB 口复用**：USB-Serial/JTAG 与 USB-OTG 共用同一物理口和 PHY，
  做 USB HID 就没法用同一根线烧写调试。本项目走 BLE，因此 USB 口保留给调试。

---

## 后续计划

- [ ] 虚拟键盘 UI（含 Ctrl/Shift 修饰键锁定、快捷键按钮）
- [ ] 触摸板（单指移动 + 点击，双指滚动）
- [ ] 三种模式切换：分屏 / 全键盘 / 全触摸板
- [ ] 挂载 `storage` 分区（12MB）存放字体与图标资源

---

## 许可证

本工程包含以下第三方组件，各自遵循其原始许可证：

- **LVGL** — MIT
- **lvgl_esp32_drivers** — MIT
- **esp_hid / esp_hid_gap** — 源自 ESP-IDF 示例，Unlicense OR CC0-1.0
- **esp32s3_soc** — 厂商（合木科技）提供的预编译库
