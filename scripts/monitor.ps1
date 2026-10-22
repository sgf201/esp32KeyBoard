<#
=============================================================================
 串口监视脚本（Windows 侧）

 采集板子日志，过滤 ANSI 颜色码，统计关键事件（启动 / 断言 / panic /
  触摸上报 / 错误行）。

 用法:
   .\monitor.ps1                    # 复位并采集 15 秒
   .\monitor.ps1 -Seconds 30        # 采集 30 秒
   .\monitor.ps1 -NoReset           # 不复位，只看当前输出
=============================================================================
#>
param(
    [string]$Port = "COM6",
    [int]$Seconds = 15,
    [switch]$NoReset,
    [string]$Python
)

$ErrorActionPreference = "Stop"

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

# 保证中文输出在 Windows 控制台正确显示
$env:PYTHONIOENCODING = "utf-8"
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)

$script = Join-Path $PSScriptRoot "monitor.py"
$mode = if ($NoReset) { "noreset" } else { "reset" }
& $Python $script $Port $Seconds $mode
