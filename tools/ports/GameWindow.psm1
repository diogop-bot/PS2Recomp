# Window helpers for driving a running port (game_ctl.ps1 and a port's own scripts): screenshots of
# the game window and key presses posted to it (the game window does not need focus for either).

Add-Type -AssemblyName System.Drawing
if (-not ('WinCapture' -as [type])) {
    Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class WinCapture {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc proc, IntPtr lParam);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int cmd);

    // Largest top-level window of a process (MainWindowHandle can come back empty).
    public static IntPtr LargestWindow(uint pid, out bool visible, out bool minimized) {
        IntPtr best = IntPtr.Zero; long bestArea = -1; bool vis = false, min = false;
        EnumWindows((h, l) => {
            uint owner; GetWindowThreadProcessId(h, out owner);
            if (owner == pid) {
                RECT r; GetClientRect(h, out r);
                long area = (long)(r.Right - r.Left) * (r.Bottom - r.Top);
                if (area > bestArea) { bestArea = area; best = h; vis = IsWindowVisible(h); min = IsIconic(h); }
            }
            return true;
        }, IntPtr.Zero);
        visible = vis; minimized = min;
        return best;
    }
}
"@
}

# Key names -> virtual key, scancode (0x100 = extended key): letters, digits, F1-F12 and the usual
# named keys. Which pad button each key is depends on the port's input mapping.
$GameKeys = @{
    "Enter" = @(0x0D, 0x1C); "Escape" = @(0x1B, 0x01); "Space" = @(0x20, 0x39); "Tab" = @(0x09, 0x0F)
    "Backspace" = @(0x08, 0x0E); "LShift" = @(0xA0, 0x2A); "RShift" = @(0xA1, 0x36); "LCtrl" = @(0xA2, 0x1D)
    "Up" = @(0x26, 0x148); "Down" = @(0x28, 0x150); "Left" = @(0x25, 0x14B); "Right" = @(0x27, 0x14D)
}
$letterScancodes = @{
    "Q" = 0x10; "W" = 0x11; "E" = 0x12; "R" = 0x13; "T" = 0x14; "Y" = 0x15; "U" = 0x16; "I" = 0x17; "O" = 0x18; "P" = 0x19
    "A" = 0x1E; "S" = 0x1F; "D" = 0x20; "F" = 0x21; "G" = 0x22; "H" = 0x23; "J" = 0x24; "K" = 0x25; "L" = 0x26
    "Z" = 0x2C; "X" = 0x2D; "C" = 0x2E; "V" = 0x2F; "B" = 0x30; "N" = 0x31; "M" = 0x32
}
foreach ($letter in $letterScancodes.Keys) { $GameKeys[$letter] = @([int][char]$letter, $letterScancodes[$letter]) }
for ($d = 1; $d -le 9; $d++) { $GameKeys["$d"] = @((0x30 + $d), (0x01 + $d)) }
$GameKeys["0"] = @(0x30, 0x0B)
for ($f = 1; $f -le 10; $f++) { $GameKeys["F$f"] = @((0x6F + $f), (0x3A + $f)) }
$GameKeys["F11"] = @(0x7A, 0x57); $GameKeys["F12"] = @(0x7B, 0x58)

function Get-GameWindow([System.Diagnostics.Process]$Process) {
    $Process.Refresh()
    if ($Process.MainWindowHandle -ne [IntPtr]::Zero) { return $Process.MainWindowHandle }
    $visible = $false; $minimized = $false
    return [WinCapture]::LargestWindow([uint32]$Process.Id, [ref]$visible, [ref]$minimized)
}

# Diagnostic: describes the game's top-level window (handle, visible, minimized, client size).
function Get-GameWindowInfo([System.Diagnostics.Process]$Process) {
    $Process.Refresh()
    $visible = $false; $minimized = $false
    $hwnd = [WinCapture]::LargestWindow([uint32]$Process.Id, [ref]$visible, [ref]$minimized)
    $rect = New-Object WinCapture+RECT
    [WinCapture]::GetClientRect($hwnd, [ref]$rect) | Out-Null
    $main = $Process.MainWindowHandle
    $mainRect = New-Object WinCapture+RECT
    [WinCapture]::GetClientRect($main, [ref]$mainRect) | Out-Null
    "main=$main minimized=$([WinCapture]::IsIconic($main)) size=$($mainRect.Right)x$($mainRect.Bottom) title='$($Process.MainWindowTitle)'; " +
        "largest=$hwnd visible=$visible size=$($rect.Right)x$($rect.Bottom)"
}

# Restores a minimized game window without activating it (SW_SHOWNOACTIVATE), so captures work.
function Restore-GameWindow([System.Diagnostics.Process]$Process) {
    $hwnd = Get-GameWindow $Process
    if ([WinCapture]::IsIconic($hwnd)) { [WinCapture]::ShowWindow($hwnd, 4) | Out-Null; return $true }
    return $false
}

# PW_CLIENTONLY | PW_RENDERFULLCONTENT: grabs the window's own content even when it is covered
function Save-GameWindowImage([System.Diagnostics.Process]$Process, [string]$Path) {
    $hwnd = Get-GameWindow $Process
    if ($hwnd -eq [IntPtr]::Zero) { return $false }
    $rect = New-Object WinCapture+RECT
    [WinCapture]::GetClientRect($hwnd, [ref]$rect) | Out-Null
    $w = $rect.Right - $rect.Left; $h = $rect.Bottom - $rect.Top
    if ($w -le 0 -or $h -le 0) { return $false }
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [WinCapture]::PrintWindow($hwnd, $hdc, 3) | Out-Null
    $g.ReleaseHdc($hdc); $g.Dispose()
    $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    return $true
}

# GLFW maps keys from the scancode in lParam, so post WM_KEYDOWN/WM_KEYUP with it set.
# The key is held for HoldMs because the game samples input once per (slow) frame.
function Send-GameKey([System.Diagnostics.Process]$Process, [string]$Name, [int]$HoldMs) {
    if (-not $GameKeys.ContainsKey($Name)) { throw "unknown key '$Name' (known: $($GameKeys.Keys -join ', '))" }
    $vk, $scancode = $GameKeys[$Name]
    $hwnd = Get-GameWindow $Process
    $base = [int64]1 -bor ([int64]($scancode -band 0xFF) -shl 16)
    if ($scancode -band 0x100) { $base = $base -bor ([int64]1 -shl 24) }
    [WinCapture]::PostMessage($hwnd, 0x0100, [IntPtr]$vk, [IntPtr]$base) | Out-Null
    Start-Sleep -Milliseconds $HoldMs
    $up = $base -bor ([int64]1 -shl 30) -bor ([int64]1 -shl 31)
    [WinCapture]::PostMessage($hwnd, 0x0101, [IntPtr]$vk, [IntPtr]$up) | Out-Null
}

Export-ModuleMember -Function Get-GameWindow, Get-GameWindowInfo, Restore-GameWindow, Save-GameWindowImage, Send-GameKey -Variable GameKeys
