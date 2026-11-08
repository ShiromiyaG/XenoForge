# The standing benchmark against Xenia (docs/renderer.md, "The standing
# benchmark"): one scene that both reach without input, run by ours and by
# Xenia's Direct3D 12 backend (its best renderer, so the bar) in turn
# (A B A B ...), each timed by xfbench over the same window. One xfbench line
# a run is appended to the output file; the summary (median and spread a
# configuration) is printed at the end.
#
#   tools/bench/bench_vs_xenia.ps1 -Disc <iso> -Ours <exe> [-OursArgs ...]
#       [-Xenia <xenia_canary.exe>] [-Seconds 150] [-Warmup 90] [-Runs 3]
#       [-Configs ours,ours-host,ours-base,ours-host-base,xenia-d3d12,xenia-d3d12-rov] [-Out <file>]
#       [-Timeline] [-Scale <percent>] [-BaseEnv <variables>]
#
# -Scale draws both at a resolution scale: ours through XF_RESOLUTION_SCALE
# (any 25% step), Xenia through draw_resolution_scale_x/y (whole multiples:
# 200, 300).
#
# -Ours is a player (player_<game> or player with --cache); it runs with the
# Vulkan layer of xfbench_hook, Xenia with the DXGI hook. Disc paths stay on
# the command line: nothing here names a game.

param(
    [Parameter(Mandatory = $true)][string]$Disc,
    [Parameter(Mandatory = $true)][string]$Ours,
    [string[]]$OursArgs = @(),
    [string]$Xenia = "$PSScriptRoot\..\..\..\xenia-canary\build\bin\Windows\Release\xenia_canary.exe",
    [double]$Seconds = 150,
    [double]$Warmup = 90,
    [int]$Runs = 3,
    [string[]]$Configs = @('ours', 'xenia-d3d12'),
    [string]$Out = "bench.txt",
    [string]$Bench = "$PSScriptRoot\..\..\build\xfbench.exe",
    [switch]$Timeline,
    [int]$Scale = 100,
    # what the -base configurations turn off (each set to 1)
    [string[]]$BaseEnv = @('XF_VK_NO_DEVICE_LOCAL_DRAW_DATA', 'XF_VK_DRAW_STATE_ALWAYS')
)

$extra = @()
if ($Timeline) { $extra += '--timeline' }

# Ours runs with a config of its own unless -OursArgs names one: the user's
# config.toml may ask for a resolution scale or picture effects (a 200% scale
# with FXAA quadrupled the GPU time a frame), and the bar is Xenia at its
# defaults. The video settings are the defaults; the saves stay where the
# user's config keeps them (the shared storage beside it), so the game sees
# the same profile and saves.
if ($OursArgs -notcontains '--config') {
    $userConfig = Join-Path $env:APPDATA 'xenoforge\config.toml'
    $storage = Join-Path $env:APPDATA 'xenoforge\storage'
    if (Test-Path $userConfig) {
        $root = Select-String -Path $userConfig -Pattern "^\s*root\s*=\s*'(.+)'" | Select-Object -First 1
        if ($root) { $storage = $root.Matches[0].Groups[1].Value }
    }
    $benchConfig = "$Out.config.toml"
    @(
        '# written by bench_vs_xenia.ps1: the defaults, as Xenia runs',
        '[saves]',
        "root = '$storage'",
        '',
        '[video]',
        'anisotropy = 0',
        "antialiasing = 'none'",
        "upscaler = 'none'",
        "texture_filter = 'game'",
        'resolution_scale = 100',
        'vsync = true'
    ) | Set-Content -Path $benchConfig -Encoding utf8NoBOM
    $OursArgs = @('--config', $benchConfig) + $OursArgs
}
for ($run = 1; $run -le $Runs; $run++) {
    foreach ($config in $Configs) {
        $label = if ($Scale -eq 100) { "$config#$run" } else { "$config-$Scale#$run" }
        if ($Scale -ne 100) { $env:XF_RESOLUTION_SCALE = "$Scale" }
        $xeniaScale = @()
        if ($Scale -ne 100) { $xeniaScale = @("--draw_resolution_scale_x=$($Scale / 100)", "--draw_resolution_scale_y=$($Scale / 100)") }
        # ours: the default render target path (interlock where the device
        # has it); ours-host: host render targets. Xenia: its defaults
        # (host render targets), or -rov for its exact path.
        # A -base suffix (ours-base, ours-host-base) is the same build with the
        # renderer's optional speedups listed in $BaseEnv turned off, the
        # baseline to set them against in one run.
        $base = $config -like '*-base'
        $kind = $config -replace '-base$', ''
        $env:XF_RENDER_TARGET_PATH = if ($kind -eq 'ours-host') { 'host' } else { '' }
        if ($base) { foreach ($name in $BaseEnv) { Set-Item "Env:$name" '1' } }
        switch ($kind) {
            { $_ -in 'ours', 'ours-host' } {
                $line = & $Bench --seconds $Seconds --warmup $Warmup --vk @extra --stdout "$Out.$config.$run.log" --label $label -- $Ours $Disc @OursArgs
            }
            'xenia-d3d12' {
                $line = & $Bench --seconds $Seconds --warmup $Warmup --dxgi @extra --label $label -- $Xenia --gpu=d3d12 @xeniaScale $Disc
            }
            'xenia-d3d12-rov' {
                $line = & $Bench --seconds $Seconds --warmup $Warmup --dxgi @extra --label $label -- $Xenia --gpu=d3d12 --render_target_path_d3d12=rov @xeniaScale $Disc
            }
        }
        Remove-Item Env:XF_RENDER_TARGET_PATH -ErrorAction SilentlyContinue
        Remove-Item Env:XF_RESOLUTION_SCALE -ErrorAction SilentlyContinue
        if ($base) { foreach ($name in $BaseEnv) { Remove-Item "Env:$name" -ErrorAction SilentlyContinue } }
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
