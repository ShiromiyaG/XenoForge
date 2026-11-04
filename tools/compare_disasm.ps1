<#
.SYNOPSIS
  Checks xfshader's disassembly against Xenia's, byte for byte, for every
  shader Xenia dumped.

.DESCRIPTION
  Xenia (xenia_canary or its trace-dump, with --dump_shaders=<folder>) writes
  shader_<hash>.ucode.bin.vert|frag (the microcode, host order) and
  shader_<hash>.ucode.vert|frag (its disassembly). This runs xfshader on each
  microcode file and compares its output with Xenia's text. The dumps hold
  game data: keep them outside the repository.

.EXAMPLE
  tools\compare_disasm.ps1 -Shaders ..\traces\shaders -Xfshader build\xfshader.exe
#>
param(
    [Parameter(Mandatory)] [string]$Shaders,
    [string]$Xfshader = "$PSScriptRoot\..\build\xfshader.exe"
)

$ErrorActionPreference = "Stop"
$out = New-TemporaryFile
$identical = 0
$different = @()
foreach ($bin in Get-ChildItem $Shaders -Filter "*.ucode.bin.*" | Sort-Object Name) {
    $reference = Join-Path $bin.DirectoryName ($bin.Name -replace '\.ucode\.bin\.', '.ucode.')
    if (-not (Test-Path $reference)) { continue }
    # redirected to a file, the bytes stay as written (no re-encoding by PowerShell)
    $p = Start-Process -FilePath $Xfshader -ArgumentList "`"$($bin.FullName)`"" -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput $out.FullName
    $same = $p.ExitCode -eq 0 -and
        (Get-FileHash $out.FullName).Hash -eq (Get-FileHash $reference).Hash
    if ($same) { $identical++ } else { $different += $bin.Name }
}
Remove-Item $out
foreach ($name in $different) { Write-Host "differs: $name" }
Write-Host "$identical identical, $($different.Count) different"
if ($different.Count) { exit 1 }
