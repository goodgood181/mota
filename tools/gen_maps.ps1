# 校验 data/floors 下全部楼层地图文件：恰好 9 行、每行 13 字符（含边界墙）
# 注意：本脚本【只校验不写入】，避免生成器意外覆盖手写地图。
# 用法: pwsh -File tools/gen_maps.ps1
$ErrorActionPreference = "Stop"
$dir = Join-Path $PSScriptRoot "..\data\floors"
$files = Get-ChildItem -Path $dir -Filter "floor_*.txt" | Sort-Object Name
$bad = 0
foreach ($f in $files) {
  $rows = Get-Content $f.FullName
  $ok = $true
  $err = @()
  if ($rows.Count -ne 9) { $err += "行数 $($rows.Count) (应为 9)"; $ok = $false }
  for ($i = 0; $i -lt $rows.Count; $i++) {
    if ($rows[$i].Length -ne 13) {
      $err += "行$i len=$($rows[$i].Length) -> [$($rows[$i])]"
      $ok = $false
    }
  }
  if (-not $ok) {
    Write-Host "[FAIL] $($f.Name)"; $err | ForEach-Object { Write-Host "   $_" }
    $bad++
  } else {
    Write-Host "[ok]   $($f.Name) ($($rows.Count) 行)"
  }
}
if ($bad -gt 0) { exit 1 }
Write-Host "全部 $($files.Count) 个楼层文件行宽校验通过"