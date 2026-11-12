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
#       [-Timeline] [-Scale <percent>] [-BaseEnv <variables>] [-Press <buttons@seconds[:hold]>,...]
#
# -Press plays the same pad script in both (ours: --press; Xenia: keys posted
# to its window), so the window can be a played scene: say, A every 4 s
# through the menus, then rt held, measured over the drive that follows.
#
# -Scale draws both at a resolution scale: ours through XF_RESOLUTION_SCALE
# (any 25% step), Xenia through draw_resolution_scale_x/y (whole multiples:
# 200, 300).
#
# -Ours is a player (player_<game> or player with --cache); it records its own
# presents (xfbench --self: no layer in it), Xenia runs with the DXGI hook. Disc paths stay on
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
    [string[]]$BaseEnv = @('XF_VK_NO_DEVICE_LOCAL_DRAW_DATA', 'XF_VK_DRAW_STATE_ALWAYS'),
    # pad presses from the program's start, the player's --press syntax
    # (buttons joined by +, @seconds, :hold seconds): a, b, x, y, start, back,
    # lb, rb, lt, rt. Ours gets them as --press; Xenia as keys posted to its
    # window (--hid=keyboard --keyboard_mode=1, its default bindings), which
    # need no focus, so nothing else on the desktop gets them. The scene a
    # benchmark measures can then be one that is played, not a title screen.
    [string[]]$Press = @(),
    # more arguments for Xenia (a measuring build's switches, say)
    [string[]]$XeniaArgs = @(),
    # the GPU's graphics clock every 250 ms of each run, beside its log
    # (nvidia-smi): GPU time a frame counts at whatever clock the driver
    # chose, and a lightly loaded GPU is often clocked down
    [switch]$Clocks
)

$extra = @()
if ($Timeline) { $extra += '--timeline' }
# (whole paths: xfbench starts the program from the process's directory,
# which is not PowerShell's location)
$Ours = (Resolve-Path $Ours).Path
$Bench = (Resolve-Path $Bench).Path
if (Test-Path $Xenia) { $Xenia = (Resolve-Path $Xenia).Path }
if (-not [System.IO.Path]::IsPathRooted($Out)) { $Out = Join-Path (Get-Location).Path $Out }

# The presses as key events for Xenia's keyboard driver: (seconds, key, down)
$xeniaKeys = @{ a = 0xBA; b = 0xDE; x = 0x4C; y = 0x50; start = 0x58; back = 0x5A; lb = 0x31; rb = 0x33; lt = 0x51; rt = 0x45 }
$keyEvents = @()
$pressArgs = @()
foreach ($p in $Press) {
    if ($p -notmatch '^([a-z+]+)@([0-9.]+)s?(?::([0-9.]+)s?)?$') { throw "-Press ${p}: not buttons@seconds[:hold]" }
    $at = [double]$Matches[2]; $hold = if ($Matches[3]) { [double]$Matches[3] } else { 0.3 }
    foreach ($name in $Matches[1] -split '\+') {
        if (-not $xeniaKeys.ContainsKey($name)) { throw "-Press ${p}: no key for $name in Xenia's keyboard bindings" }
        $keyEvents += [pscustomobject]@{ t = $at; vk = $xeniaKeys[$name]; down = $true }
        $keyEvents += [pscustomobject]@{ t = $at + $hold; vk = $xeniaKeys[$name]; down = $false }
    }
    $pressArgs += @('--press', $p)
}
$keyEvents = @($keyEvents | Sort-Object t)
if ($keyEvents.Count) {
    Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class XfBenchKeys {
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    public static void Post(IntPtr h, int vk, bool down) {
        if (down) PostMessage(h, 0x0100, (IntPtr)vk, (IntPtr)1);
        else PostMessage(h, 0x0101, (IntPtr)vk, unchecked((IntPtr)(int)0xC0000001));
    }
}
"@
}

# Xenia under xfbench with the presses sent at their times; xfbench's lines
function Invoke-XeniaPressed([string[]]$benchArgs) {
    $tmp = [System.IO.Path]::GetTempFileName()
    $quoted = $benchArgs | ForEach-Object { '"' + $_ + '"' }
    $proc = Start-Process -FilePath $Bench -ArgumentList $quoted -RedirectStandardOutput $tmp -PassThru -NoNewWindow
    $start = Get-Date
    foreach ($e in $keyEvents) {
        while (((Get-Date) - $start).TotalSeconds -lt $e.t -and -not $proc.HasExited) { Start-Sleep -Milliseconds 20 }
        $x = Get-Process xenia_canary -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($x -and $x.MainWindowHandle -ne [IntPtr]::Zero) { [XfBenchKeys]::Post($x.MainWindowHandle, $e.vk, $e.down) }
    }
    $proc.WaitForExit()
    $lines = Get-Content $tmp
    Remove-Item $tmp -ErrorAction SilentlyContinue
    return $lines
}

# Ours runs with a config of its own unless -OursArgs names one: the user's
# config.toml may ask for a resolution scale or picture effects (a 200% scale
# with FXAA quadrupled the GPU time a frame), and the bar is Xenia at its
# defaults. The video settings are the defaults, and the saves a storage of
# the benchmark's own beside the output (never the user's: a played script
# would write into the user's saves, and a game with the user's progress may
# go to another scene than the script expects), made fresh for each run.
$storage = $null
if ($OursArgs -notcontains '--config') {
    $storage = [System.IO.Path]::GetFullPath("$Out.storage")
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
        # (forced either way: the player's own default is its video.edram setting)
        $env:XF_RENDER_TARGET_PATH = if ($kind -eq 'ours-host') { 'host' } else { 'fsi' }
        # (each NAME set to 1, or NAME=VALUE to that value)
        if ($base) {
            foreach ($name in $BaseEnv) {
                $pair = $name -split '=', 2
                Set-Item "Env:$($pair[0])" $(if ($pair.Count -eq 2) { $pair[1] } else { '1' })
            }
        }
        if ($storage) { Remove-Item -Recurse -Force $storage -ErrorAction SilentlyContinue }
        $clockLog = $null
        if ($Clocks) {
            $clockLog = Start-Process -FilePath nvidia-smi -ArgumentList '--query-gpu=clocks.gr --format=csv,noheader,nounits -lms 250' `
                -RedirectStandardOutput "$Out.$config.$run.clocks.txt" -PassThru -WindowStyle Hidden
        }
        switch ($kind) {
            { $_ -in 'ours', 'ours-host' } {
                $line = & $Bench --seconds $Seconds --warmup $Warmup --self @extra --stdout "$Out.$config.$run.log" --label $label -- $Ours $Disc @OursArgs @pressArgs
            }
            { $_ -in 'xenia-d3d12', 'xenia-d3d12-rov' } {
                $xeniaArgs = @('--gpu=d3d12') + $xeniaScale + $XeniaArgs
                if ($kind -eq 'xenia-d3d12-rov') { $xeniaArgs += '--render_target_path_d3d12=rov' }
                $benchArgs = @('--seconds', "$Seconds", '--warmup', "$Warmup", '--dxgi') + $extra + @('--label', $label, '--', $Xenia)
                if ($keyEvents.Count) {
                    $line = Invoke-XeniaPressed ($benchArgs + $xeniaArgs + @('--hid=keyboard', '--keyboard_mode=1', $Disc))
                } else {
                    $line = & $Bench @benchArgs @xeniaArgs $Disc
                }
            }
        }
        Remove-Item Env:XF_RENDER_TARGET_PATH -ErrorAction SilentlyContinue
        Remove-Item Env:XF_RESOLUTION_SCALE -ErrorAction SilentlyContinue
        if ($base) { foreach ($name in $BaseEnv) { Remove-Item "Env:$(($name -split '=', 2)[0])" -ErrorAction SilentlyContinue } }
        if ($clockLog) {
            Stop-Process -Id $clockLog.Id -ErrorAction SilentlyContinue
            # (the window's samples: four a second from the start)
            $mhz = @(Get-Content "$Out.$config.$run.clocks.txt" | Where-Object { $_ -match '^\s*\d+\s*$' } | ForEach-Object { [int]$_ } |
                Select-Object -Skip ([int]($Warmup * 4)) -First ([int]($Seconds * 4)))
            if ($mhz.Count) {
                $sorted = $mhz | Sort-Object
                $line = @($line | Select-Object -SkipLast 1) + "$(@($line)[-1]) gpu_clock_mhz_median=$($sorted[[int][Math]::Floor(($sorted.Count - 1) / 2)])"
            }
        }
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
