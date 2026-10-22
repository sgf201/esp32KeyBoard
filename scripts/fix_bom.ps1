<#
  给仓库里的 .ps1 脚本补 UTF-8 BOM。

  Windows PowerShell 5.1 读取无 BOM 的 UTF-8 文件时会按系统代码页解析，
  中文字符会破坏语法，报 "字符串缺少终止符" 之类的错误。
  编辑 .ps1 后跑一次本脚本即可。
#>
$root = Split-Path $PSScriptRoot -Parent
$files = Get-ChildItem -Path $root -Recurse -Filter *.ps1 -File

$bom = New-Object System.Text.UTF8Encoding($true)
foreach ($f in $files) {
    $text = [System.IO.File]::ReadAllText($f.FullName, [System.Text.UTF8Encoding]::new($false))
    if ($text.Length -gt 0 -and $text[0] -eq [char]0xFEFF) {
        $text = $text.Substring(1)
    }
    [System.IO.File]::WriteAllText($f.FullName, $text, $bom)
    Write-Host "  BOM ok: $($f.FullName.Replace($root, '.'))"
}
Write-Host "`n完成，共处理 $($files.Count) 个文件。"
