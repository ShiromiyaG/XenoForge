# The standing benchmark against Xenia (docs/renderer.md, "The standing
# benchmark"): one scene that both reach without input, run by ours and by
# Xenia's Direct3D 12 and Vulkan backends in turn (A B C A B C ...), each
# timed by xfbench over the same window. One xfbench line a run is appended
# to the output file; the summary (median and spread a configuration) is
# printed at the end.
#
#   tools/bench/bench_vs_xenia.ps1 -Disc <iso> -Ours <exe> [-OursArgs ...]
#       [-Xenia <xenia_canary.exe>] [-Seconds 150] [-Warmup 90] [-Runs 3]
#       [-Configs ours,xenia-d3d12,xenia-vulkan] [-Out <file>] [-Timeline]
#
# -Ours is a player (player_<game> or player with --cache); it runs with the
# Vulkan layer of xfbench_hook, Xenia's Direct3D 12 backend with the DXGI
# hook. Disc paths stay on the command line: nothing here names a game.

param(
    [Parameter(Mandatory = $true)][string]$Disc,
    [Parameter(Mandatory = $true)][string]$Ours,
    [string[]]$OursArgs = @(),
    [string]$Xenia = "$PSScriptRoot\..\..\..\xenia-canary\build\bin\Windows\Release\xenia_canary.exe",
    [double]$Seconds = 150,
    [double]$Warmup = 90,
    [int]$Runs = 3,
    [string[]]$Configs = @('ours', 'xenia-d3d12', 'xenia-vulkan'),
    [string]$Out = "bench.txt",
    [string]$Bench = "$PSScriptRoot\..\..\build\xfbench.exe",
    [switch]$Timeline
)

$extra = @()
if ($Timeline) { $extra += '--timeline' }
for ($run = 1; $run -le $Runs; $run++) {
    foreach ($config in $Configs) {
        $label = "$config#$run"
        # ours: the default render target path (interlock where the device
        # has it); ours-host: host render targets. Xenia: its defaults
        # (host render targets), or -rov / -fsi for its exact paths.
        $env:XF_RENDER_TARGET_PATH = if ($config -eq 'ours-host') { 'host' } else { '' }
        switch ($config) {
            { $_ -in 'ours', 'ours-host' } {
                $line = & $Bench --seconds $Seconds --warmup $Warmup --vk @extra --stdout "$Out.$config.$run.log" --label $label -- $Ours $Disc @OursArgs
            }
            'xenia-d3d12' {
                $line = & $Bench --seconds $Seconds --warmup $Warmup --dxgi @extra --label $label -- $Xenia --gpu=d3d12 $Disc
            }
            'xenia-d3d12-rov' {
                $line = & $Bench --seconds $Seconds --warmup $Warmup --dxgi @extra --label $label -- $Xenia --gpu=d3d12 --render_target_path_d3d12=rov $Disc
            }
            'xenia-vulkan' {
                $line = & $Bench --seconds $Seconds --warmup $Warmup --vk @extra --label $label -- $Xenia --gpu=vulkan $Disc
            }
            'xenia-vulkan-fsi' {
                $line = & $Bench --seconds $Seconds --warmup $Warmup --vk @extra --label $label -- $Xenia --gpu=vulkan --render_target_path_vulkan=fsi $Disc
            }
        }
        Remove-Item Env:XF_RENDER_TARGET_PATH -ErrorAction SilentlyContinue
        $line | Add-Content $Out
        $line | Select-Object -Last 1
    }
}

# the median and the spread of each configuration
$rows = Get-Content $Out | Where-Object { $_ -like 'label=*' } | ForEach-Object {
    $h = @{}
    foreach ($pair in $_ -split ' ') { $k, $v = $pair -split '=', 2; $h[$k] = $v }
    [pscustomobject]@{ config = ($h.label -split '#')[0]; fps = [double]$h.fps; frame = [double]$h.frame_ms_median;
        p95 = [double]$h.p95; gpu_frame = [double]$h.gpu_ms_per_frame; gpu_s = [double]$h.gpu_ms_per_s; cpu = [double]$h.cpu_s_per_s }
}
foreach ($group in $rows | Group-Object config) {
    $summary = foreach ($field in 'fps', 'frame', 'p95', 'gpu_frame', 'gpu_s', 'cpu') {
        $values = @($group.Group | ForEach-Object { $_.$field } | Sort-Object)
        $median = $values[[int][Math]::Floor(($values.Count - 1) / 2)]
        "{0}={1:N2} ({2:N2}-{3:N2})" -f $field, $median, $values[0], $values[-1]
    }
    "$($group.Name) [$($group.Count) runs]: $($summary -join ', ')"
}
