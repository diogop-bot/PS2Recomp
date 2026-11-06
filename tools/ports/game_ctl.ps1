# Drives a running instance of a recompiled game step by step. The game comes from the port's
# ps2port.json (-Port, $env:PS2PORT_CONFIG, or the first one in the current folder or above);
# the state of the live instance is in <logs>\live.json.
#   game_ctl.ps1 start [-Config RelWithDebInfo]    launch detached, stdout/stderr in <logs>
#   game_ctl.ps1 key <Name> [-HoldMs 400]          press a key (names in GameWindow.psm1)
#   game_ctl.ps1 shot [-Name label]                PNG of the game window, prints its path
#   game_ctl.ps1 status                            alive?, uptime, window, last statusPattern line
#   game_ctl.ps1 stop
# Environment variables set before "start" are inherited by the game.
param([Parameter(Mandatory = $true, Position = 0)][ValidateSet("start", "key", "shot", "status", "stop")][string]$Command,
      [Parameter(Position = 1)][string]$Arg = "", [int]$HoldMs = 400, [string]$Name = "",
      [string]$Config = "", [string]$Port = "")
$ErrorActionPreference = "Stop"
Import-Module "$PSScriptRoot\PortConfig.psm1" -Force
Import-Module "$PSScriptRoot\GameWindow.psm1"
$cfg = Get-PortConfig $Port $Config
$logDir = $cfg.Logs
$stateFile = "$logDir\live.json"

function Get-Live {
    if (-not (Test-Path $stateFile)) { throw "no live game (run: game_ctl.ps1 start)" }
    $state = Get-Content $stateFile | ConvertFrom-Json
    $process = Get-Process -Id $state.pid -ErrorAction SilentlyContinue
    if (-not $process -or $process.ProcessName -ne $cfg.ProcessName) { throw "game (pid $($state.pid)) is not running" }
    return $state, $process
}

switch ($Command) {
    "start" {
        if (-not (Test-Path $cfg.Exe)) { throw "not built: $($cfg.Exe)" }
        New-Item -ItemType Directory -Force $logDir | Out-Null
        $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
        $out = "$logDir\$stamp.out.log"; $err = "$logDir\$stamp.err.log"
        $p = Start-Process -FilePath $cfg.Exe -ArgumentList ('"' + $cfg.Elf + '"') `
            -WorkingDirectory $cfg.WorkDir -RedirectStandardOutput $out -RedirectStandardError $err -PassThru
        @{ pid = $p.Id; started = (Get-Date).ToString("o"); stamp = $stamp; out = $out; err = $err; shots = 0 } |
            ConvertTo-Json | Set-Content $stateFile
        "started pid $($p.Id); logs $err"
    }
    "key" {
        $state, $p = Get-Live
        Send-GameKey $p $Arg $HoldMs
        "pressed $Arg ($HoldMs ms)"
    }
    "shot" {
        $state, $p = Get-Live
        $state.shots++
        $label = if ($Name) { "-$Name" } else { "" }
        $path = "$logDir\$($state.stamp).live$($state.shots)$label.png"
        if (Restore-GameWindow $p) { "restored the minimized game window"; Start-Sleep -Milliseconds 500 }
        if (-not (Save-GameWindowImage $p $path)) { throw "no game window to capture" }
        $state | ConvertTo-Json | Set-Content $stateFile
        $path
    }
    "status" {
        $state, $p = Get-Live
        $uptime = [int]((Get-Date) - [datetime]$state.started).TotalSeconds
        "running pid $($state.pid), up $uptime s, $([int]($p.WorkingSet64 / 1MB)) MB"
        Get-GameWindowInfo $p
        if ($cfg.Raw.statusPattern) {
            $last = Select-String -Path $state.err -Pattern $cfg.Raw.statusPattern | Select-Object -Last 1
            if ($last) { $last.Line }
        }
    }
    "stop" {
        if (-not (Test-Path $stateFile)) { "no live game"; break }
        $state = Get-Content $stateFile | ConvertFrom-Json
        $p = Get-Process -Id $state.pid -ErrorAction SilentlyContinue
        if ($p -and $p.ProcessName -eq $cfg.ProcessName) { Stop-Process -Id $p.Id -Force; $p.WaitForExit() }
        Remove-Item $stateFile
        "stopped"
    }
}
