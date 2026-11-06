# Reads a port's ps2port.json (see README.md in this folder) for the PowerShell tools next to it.
# Found from, in order: an explicit path (-Port), $env:PS2PORT_CONFIG, or the first ps2port.json in
# the current directory or one of its parents. Paths in the file are relative to the file's folder.

function Find-PortConfig([string]$Explicit = "") {
    $path = if ($Explicit) { $Explicit } else { $env:PS2PORT_CONFIG }
    if ($path) {
        if (Test-Path $path -PathType Container) { $path = Join-Path $path "ps2port.json" }
        if (-not (Test-Path $path -PathType Leaf)) { throw "port config not found: $path" }
        return (Resolve-Path $path).Path
    }
    $here = (Get-Location).Path
    while ($here) {
        $candidate = Join-Path $here "ps2port.json"
        if (Test-Path $candidate -PathType Leaf) { return $candidate }
        $parent = Split-Path -Parent $here
        if (-not $parent -or $parent -eq $here) { break }
        $here = $parent
    }
    throw "no ps2port.json here or above (pass -Port or set PS2PORT_CONFIG)"
}

# Returns the config with resolved absolute paths: Root, Exe, Elf, WorkDir, Logs, ProcessName, Raw.
function Get-PortConfig([string]$Explicit = "", [string]$BuildConfig = "") {
    $path = Find-PortConfig $Explicit
    $raw = Get-Content -Raw -Encoding UTF8 $path | ConvertFrom-Json
    $root = Split-Path -Parent $path
    if (-not $BuildConfig) { $BuildConfig = if ($raw.buildConfig) { $raw.buildConfig } else { "RelWithDebInfo" } }
    $resolve = {
        param([string]$relative)
        if (-not $relative) { return $null }
        [System.IO.Path]::GetFullPath((Join-Path $root $relative.Replace("{config}", $BuildConfig)))
    }.GetNewClosure()
    $exe = & $resolve $raw.exe
    [pscustomobject]@{
        Path        = $path
        Root        = $root
        BuildConfig = $BuildConfig
        Exe         = $exe
        Elf         = & $resolve $raw.elf
        WorkDir     = & $resolve $(if ($raw.workdir) { $raw.workdir } else { "." })
        Logs        = & $resolve $(if ($raw.logs) { $raw.logs } else { "build/runs" })
        ProcessName = [System.IO.Path]::GetFileNameWithoutExtension($exe)
        Raw         = $raw
        Resolve     = $resolve
    }
}

Export-ModuleMember -Function Find-PortConfig, Get-PortConfig
