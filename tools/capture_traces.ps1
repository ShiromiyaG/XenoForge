<#
.SYNOPSIS
  Records GPU traces of a game with xenia-canary, without input: the frames
  asked for (counted in swaps), then closes Xenia.

.DESCRIPTION
  Needs a xenia-canary built with -DXENIA_ENABLE_GPU_TRACE=ON (release
  downloads leave the trace writer out). Traces hold game data: keep them
  outside the repository.

.EXAMPLE
  tools\capture_traces.ps1 -Xenia ..\xenia-canary\build\bin\Windows\Release\xenia_canary.exe `
      -Game "D:\games\game.iso" -Out ..\traces\game -Frames 300,900,1800,3000
#>
param(
    [Parameter(Mandatory)] [string]$Xenia,
    [Parameter(Mandatory)] [string]$Game,
    [Parameter(Mandatory)] [string]$Out,
    [int[]]$Frames = @(300, 900, 1800, 3000),
    [int]$TimeoutSeconds = 180,
    [string[]]$ExtraArgs = @()
)

$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force $Out | Out-Null
# no trailing backslash: before the closing quote it would escape it
$outFull = (Resolve-Path $Out).Path.TrimEnd('\')
$frameList = ($Frames | ForEach-Object { "$_" }) -join ","

$xeniaArgs = @("--trace_gpu_prefix=$outFull", "--trace_gpu_frames=$frameList") + $ExtraArgs + @((Resolve-Path $Game).Path)
Write-Host "xenia: $Xenia $($xeniaArgs -join ' ')"
$process = Start-Process -FilePath $Xenia -ArgumentList ($xeniaArgs | ForEach-Object { "`"$_`"" }) -PassThru

# a trace is done when Xenia has moved past its frame: wait for the last one
# to appear and stop growing
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
$last = $Frames | Sort-Object | Select-Object -Last 1
$lastSize = -1
while ((Get-Date) -lt $deadline -and -not $process.HasExited) {
    Start-Sleep -Seconds 2
    $file = Get-ChildItem $outFull -Filter "*_$last.xtr" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($file) {
        if ($file.Length -eq $lastSize) { break }
        $lastSize = $file.Length
    }
}
if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }

Get-ChildItem $outFull -Filter "*.xtr" | Sort-Object Name |
    ForEach-Object { "{0,12:N0}  {1}" -f $_.Length, $_.Name }
