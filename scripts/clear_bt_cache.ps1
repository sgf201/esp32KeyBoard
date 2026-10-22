<#
  清理 ESP32 宏键盘在 Windows 里的蓝牙配对缓存。

  为什么需要：
  反复烧写固件（尤其带 Erase）会让设备端 NVS 里的绑定密钥丢失，但 Windows
  仍缓存旧密钥。此后每次连接双方都用不匹配的密钥做加密，握手失败，Windows
  主动断开（HCI 0x13 "remote user terminated connection"），表现为
  "能连上但一直掉线"。而 设置界面里"删除设备"并不总能清掉 BTHPORT 下的
  低层缓存，所以需要直接删注册表项。

  必须以管理员身份运行。

  用法:
     # 先查出设备的 MAC（串口日志里能看到，如 cc:8d:a2:ed:b3:8a）
     .\clear_bt_cache.ps1 -Mac cc8da2edb38a
#>
param(
    [Parameter(Mandatory = $false)]
    [string]$Mac = "cc8da2edb38a"
)

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isAdmin) {
    Write-Host "需要管理员权限。" -ForegroundColor Red
    Write-Host "请右键 PowerShell -> 以管理员身份运行，再执行本脚本。" -ForegroundColor Red
    exit 1
}

$mac = $Mac -replace '[:\.-]', ''

$targets = @(
    "HKLM:\SYSTEM\CurrentControlSet\Services\BTHPORT\Parameters\Devices\$mac",
    "HKLM:\SYSTEM\CurrentControlSet\Services\BTHLEEnum\Parameters\Devices\$mac",
    "HKLM:\SYSTEM\CurrentControlSet\Services\BTHPORT\Parameters\Keys\$mac"
)

Write-Host "清理设备 $mac 的配对缓存 ..." -ForegroundColor Cyan
$removed = 0
foreach ($t in $targets) {
    if (Test-Path $t) {
        try {
            Remove-Item -Path $t -Recurse -Force -ErrorAction Stop
            Write-Host "  已删除: $t" -ForegroundColor Green
            $removed++
        } catch {
            Write-Host "  删除失败: $t -> $($_.Exception.Message)" -ForegroundColor Yellow
        }
    } else {
        Write-Host "  不存在: $t" -ForegroundColor DarkGray
    }
}

Write-Host "`n删除 $removed 个注册表项。" -ForegroundColor Cyan
Write-Host "接着重启蓝牙服务：" -ForegroundColor Cyan
Write-Host "  Restart-Service bthserv -Force" -ForegroundColor White
Write-Host "然后回 设置 -> 蓝牙 重新配对设备。" -ForegroundColor White
