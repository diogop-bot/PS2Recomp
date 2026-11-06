# Builds vu1_bench.exe from two generated VU1 programs (old and new), for comparing generator changes.
#   build.ps1 -Old <old program.cpp> [-New <new program.cpp>] [-OutDir <folder>] [-Port ps2port.json]
#   build.ps1 -MainOnly        relink after changing vu1_bench.cpp only (the programs are not rebuilt)
# -New defaults to the port's vu1.program, -OutDir to build/vu1_bench under the port's root.
# The old side takes ps2x_vu1_gen.h from <OutDir>\old_include when that exists (the header the old
# program was generated for); the new side uses the one next to vu1_recompile.py.
param([string]$Old = "", [string]$New = "", [string]$OutDir = "", [string]$Port = "", [switch]$MainOnly)
$ErrorActionPreference = "Stop"
$modelDir = Split-Path -Parent $PSScriptRoot        # ps2x_vu1_gen.h
if (-not $New -or -not $OutDir) {
    Import-Module "$(Split-Path -Parent $modelDir)\PortConfig.psm1" -Force
    $cfg = Get-PortConfig $Port
    if (-not $New) {
        if (-not $cfg.Raw.vu1 -or -not $cfg.Raw.vu1.program) { throw "pass -New, or set vu1.program in $($cfg.Path)" }
        $New = & $cfg.Resolve $cfg.Raw.vu1.program
    }
    if (-not $OutDir) { $OutDir = Join-Path $cfg.Root "build\vu1_bench" }
}
$out = [System.IO.Path]::GetFullPath($OutDir)
New-Item -ItemType Directory -Force $out | Out-Null
if ($MainOnly) { $Old = $New }  # programs are not rebuilt
elseif (-not $Old) { throw "-Old <program.cpp> is required" }
$Old = (Resolve-Path $Old).Path; $New = (Resolve-Path $New).Path

$vcvars = $null
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($vs) { $vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvarsall.bat" }
}
if (-not $vcvars -or -not (Test-Path $vcvars)) { throw "Visual Studio with the C++ x64 tools not found" }

$flags = "/nologo /c /EHsc /std:c++20 /O2 /Ob2 /Oi /arch:AVX2 /fp:precise /bigobj /I`"$modelDir`" /I`"$PSScriptRoot`""
$oldInc = if (Test-Path "$out\old_include\ps2x_vu1_gen.h") { "/I`"$out\old_include`" " } else { "" }
$cmds = @(
    "cl $oldInc$flags /Dvu1gen=vu1old /Fo`"$out\old_program.obj`" `"$Old`"",
    "cl $oldInc$flags /Dvu1gen=vu1old /DBENCH_SIDE=runOld /DBENCH_TIME=timeOld /Fo`"$out\old_side.obj`" `"$PSScriptRoot\vu1_bench_side.cpp`"",
    "cl $flags /Dvu1gen=vu1new /Fo`"$out\new_program.obj`" `"$New`"",
    "cl $flags /Dvu1gen=vu1new /DBENCH_SIDE=runNew /DBENCH_TIME=timeNew /Fo`"$out\new_side.obj`" `"$PSScriptRoot\vu1_bench_side.cpp`"",
    "cl $flags /Fo`"$out\vu1_bench.obj`" `"$PSScriptRoot\vu1_bench.cpp`"",
    "link /nologo /OUT:`"$out\vu1_bench.exe`" `"$out\old_program.obj`" `"$out\old_side.obj`" `"$out\new_program.obj`" `"$out\new_side.obj`" `"$out\vu1_bench.obj`""
)
if ($MainOnly) { $cmds = $cmds | Where-Object { $_ -notmatch "_program.obj`" `"" -or $_ -match "^link" } }
$script = ($cmds | ForEach-Object { "$_ || exit /b 1" }) -join "`r`n"
$bat = "$out\build.bat"
"@echo off`r`ncall `"$vcvars`" x64 >nul`r`n$script" | Set-Content -Encoding ascii $bat
cmd /c $bat
if ($LASTEXITCODE -ne 0) { throw "build failed" }
"built $out\vu1_bench.exe"
