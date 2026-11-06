# Decompiles functions from the port's Ghidra project into a text file (read-only; the project is
# not modified). The project comes from the "ghidra" section of ps2port.json (see README.md).
# Usage: powershell -ExecutionPolicy Bypass -File decompile.ps1 [-Port ps2port.json] <out.c> 0x142b28 0x2944d8 ...
# Needs GHIDRA_INSTALL_DIR. Any address inside a function works.
param([Parameter(Mandatory = $true, Position = 0)][string]$Out, [string]$Port = "",
      [Parameter(ValueFromRemainingArguments = $true)][string[]]$Addresses)
$ErrorActionPreference = "Stop"
Import-Module "$PSScriptRoot\PortConfig.psm1" -Force
$cfg = Get-PortConfig $Port
$ghidra = $cfg.Raw.ghidra
if (-not $ghidra -or -not $ghidra.project) { throw "ps2port.json has no ghidra.project" }
if (-not $env:GHIDRA_INSTALL_DIR) { throw "GHIDRA_INSTALL_DIR is not set" }
$project = & $cfg.Resolve $ghidra.project          # <folder>\<name>, the .gpr without its extension
$projectDir = Split-Path -Parent $project
$projectName = Split-Path -Leaf $project
$program = if ($ghidra.program) { $ghidra.program } else { Split-Path -Leaf $cfg.Elf }
$scriptPath = @("$PSScriptRoot\ghidra") + @($ghidra.scripts | Where-Object { $_ } | ForEach-Object { & $cfg.Resolve $_ })

$outPath = [System.IO.Path]::GetFullPath($Out)
New-Item -ItemType Directory -Force (Split-Path -Parent $outPath) | Out-Null
if (Test-Path $outPath) { Remove-Item $outPath }
$arguments = @($projectDir, $projectName, "-process", $program, "-noanalysis", "-readOnly",
    "-scriptPath", ($scriptPath -join ';'), "-postScript", "DecompileFunctions.java", $outPath) + $Addresses
$argLine = ($arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
$log = "$projectDir\decompile.out"
$p = Start-Process -FilePath "$env:GHIDRA_INSTALL_DIR\support\analyzeHeadless.bat" -ArgumentList $argLine `
    -NoNewWindow -Wait -PassThru -RedirectStandardOutput $log -RedirectStandardError "$log.stderr"
# analyzeHeadless exits 0 even when the script throws, so check the output itself
if ($p.ExitCode -ne 0 -or -not (Test-Path $outPath)) { throw "decompile failed, see $log" }
"wrote $outPath"
