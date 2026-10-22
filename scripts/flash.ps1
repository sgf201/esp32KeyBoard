<#
=============================================================================
 ESP32-S3 宏键盘 —— 烧写脚本（Windows 侧）

 开发板通过 ESP32-S3 原生 USB-Serial/JTAG 接入（默认 COM6）。
 WSL 未接管 USB 设备，因此烧写在 Windows 侧用 esptool 完成，
 配合 WSL 中的编译产物。

 用法:
   .\flash.ps1                 # 烧到 COM6
   .\flash.ps1 -Port COM5      # 指定串口
   .\flash.ps1 -Erase          # 先全片擦除（会清除 BLE 绑定，慎用！）

 注意:
   -Erase 会清掉 NVS 里的 BLE 绑定密钥。之后必须在电脑上删除设备并
   重新配对，否则会因为密钥不匹配而反复断连。调试时通常不需要 -Erase。
=============================================================================
#>
param(
    [string]$Port = "COM6",
    [switch]$Erase,
    [string]$Python,
    [string]$BuildDir = ""
)

$ErrorActionPreference = "Stop"

# ---- 定位 Python -----------------------------------------------------------
if (-not $Python) {
    $candidates = @(
        "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe",
        "$env:LOCALAPPDATA\Programs\Python\Python311\python.exe"
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { $Python = $c; break }
    }
    if (-not $Python) {
        $cmd = Get-Command python -ErrorAction SilentlyContinue
        if ($cmd) { $Python = $cmd.Source }
    }
}
if (-not $Python -or -not (Test-Path $Python)) {
    throw "找不到 Python，请用 -Python 指定 python.exe 路径"
}

# ---- WSL 构建目录 ----------------------------------------------------------
if (-not $BuildDir) {
    $BuildDir = (wsl.exe -e bash -lc 'echo $HOME').Trim() + "/ws/esp32KeyBoard-build"
}

$Out = Join-Path (Get-Location) "build_out"

# ---- 1. 从 WSL 取出固件 ----------------------------------------------------
Write-Host "[1/3] 从 WSL 构建目录取出固件 ..." -ForegroundColor Cyan
$wslOut = (wsl.exe -e bash -lc "wslpath -u '$Out'").Trim()
wsl.exe -e bash -c @"
set -e
mkdir -p '$wslOut/partition_table'
cp -f '$BuildDir/build/hello.bin'                        '$wslOut/hello.bin'
cp -f '$BuildDir/build/bootloader/bootloader.bin'        '$wslOut/bootloader.bin'
cp -f '$BuildDir/build/partition_table/partition-table.bin' '$wslOut/partition_table/partition-table.bin'
"@
if ($LASTEXITCODE -ne 0) {
    throw "取固件失败，请先运行 scripts/build.sh（构建目录: $BuildDir）"
}

$app = Get-Item "$Out\hello.bin"
Write-Host ("      hello.bin  {0,9:N0} bytes  {1}" -f $app.Length, $app.LastWriteTime)

# ---- 2. 可选擦除 -----------------------------------------------------------
if ($Erase) {
    Write-Host "[2/3] 全片擦除（会清除 BLE 绑定！）..." -ForegroundColor Yellow
    & $Python -m esptool --chip esp32s3 -p $Port erase-flash
    if ($LASTEXITCODE -ne 0) { throw "擦除失败" }
} else {
    Write-Host "[2/3] 跳过擦除（加 -Erase 可全片擦除）" -ForegroundColor DarkGray
}

# ---- 3. 烧写 ---------------------------------------------------------------
Write-Host "[3/3] 烧写固件到 $Port ..." -ForegroundColor Cyan
$args = @(
    "-m", "esptool", "--chip", "esp32s3", "-p", $Port, "-b", "460800",
    "--before", "default-reset", "--after", "hard-reset",
    "write-flash", "--flash-mode", "dio", "--flash-freq", "80m", "--flash-size", "16MB",
    "0x0",     "$Out\bootloader.bin",
    "0x8000",  "$Out\partition_table\partition-table.bin",
    "0x10000", "$Out\hello.bin"
)
$log = & $Python @args 2>&1
$rc = $LASTEXITCODE
$log | Select-String -Pattern "Wrote |Hash of data|error|Error|failed" | ForEach-Object { "      " + $_.Line }

if ($rc -ne 0) { throw "烧写失败 (exit $rc)" }
Write-Host "`n烧写完成，板子已复位。" -ForegroundColor Green
Write-Host "查看日志:  .\scripts\monitor.ps1 -Port $Port" -ForegroundColor Green
