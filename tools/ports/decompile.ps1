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
# Ghidra writes into the project folder; the result is moved to $Out afterwards.
$tmpOut = "$projectDir\decompile.tmp.c"
if (Test-Path $tmpOut) { Remove-Item $tmpOut }
$log = "$projectDir\decompile.out"

# analyzeHeadless.bat breaks on parentheses in its arguments ("... was unexpected at this time"),
# and drives without 8.3 names offer no way around them. If any path has them, Ghidra sees the port
# root through a temporary subst drive (removed afterwards).
$ghidraPaths = @($projectDir, $tmpOut) + $scriptPath
$drive = $null
if ($ghidraPaths | Where-Object { $_ -match '[()]' }) {
    $drive = [char[]](90..71) | Where-Object { -not (Test-Path "${_}:\") } | Select-Object -First 1
    if (-not $drive) { throw "no free drive letter for subst" }
    subst "${drive}:" $cfg.Root
    if ($LASTEXITCODE -ne 0) { throw "subst ${drive}: failed" }
    $rootPrefix = $cfg.Root.TrimEnd('\')
    $mapPath = {
        param([string]$path)
        if ($path.StartsWith($rootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) { "${drive}:" + $path.Substring($rootPrefix.Length) }
        else { $path }
    }
    $projectDirArg = & $mapPath $projectDir
    $tmpOutArg = & $mapPath $tmpOut
    $scriptPathArg = @($scriptPath | ForEach-Object { & $mapPath $_ })
    if (@($projectDirArg, $tmpOutArg) + $scriptPathArg | Where-Object { $_ -match '[()]' }) {
        subst "${drive}:" /D
        throw "paths outside the port root have parentheses: $(($ghidraPaths | Where-Object { $_ -match '[()]' }) -join ', ')"
    }
} else {
    $projectDirArg = $projectDir; $tmpOutArg = $tmpOut; $scriptPathArg = $scriptPath
}
try {
    $arguments = @($projectDirArg, $projectName, "-process", $program, "-noanalysis", "-readOnly",
        "-scriptPath", ($scriptPathArg -join ';'), "-postScript", "DecompileFunctions.java", $tmpOutArg) + $Addresses
    $argLine = ($arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $p = Start-Process -FilePath "$env:GHIDRA_INSTALL_DIR\support\analyzeHeadless.bat" -ArgumentList $argLine `
        -NoNewWindow -Wait -PassThru -RedirectStandardOutput $log -RedirectStandardError "$log.stderr"
} finally {
    if ($drive) { subst "${drive}:" /D }
}
# analyzeHeadless exits 0 even when the script throws, so check the output itself
if ($p.ExitCode -ne 0 -or -not (Test-Path $tmpOut)) { throw "decompile failed, see $log" }
Move-Item $tmpOut $outPath
"wrote $outPath"
