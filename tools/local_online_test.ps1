# Test the online mode on ONE PC: starts a host and a joiner of the game, side by side (Riven).
#
#   powershell -ExecutionPolicy Bypass -File tools\local_online_test.ps1 [-Rom path\to\baserom.gba] [-Port 47900] [-Delay 0]
#
# It uses the game build that the launcher prepared (so open KirbyRecomp.exe once first), gives each
# instance its own save and settings (nothing of your real save/ini is touched) and mutes the second one.
# Keyboard keys go to the window that has focus; a connected controller drives BOTH windows (unplug it or
# play with the keyboard only).
param(
    [string]$Rom = '',
    [int]$Port = 47900,
    [string]$Delay = '0'
)
$ErrorActionPreference = 'Stop'
$base = Join-Path $env:LOCALAPPDATA 'KirbyRecomp'
# newest game exe (interpreter or compiled build) that knows the online options; an older one is skipped
$exe = Get-ChildItem (Join-Path $base '*\interp\kirby.exe'), (Join-Path $base '*\kirby.exe') -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Where-Object {
        [System.Text.Encoding]::GetEncoding(28591).GetString([System.IO.File]::ReadAllBytes($_.FullName)).Contains('link-required')
    } | Select-Object -First 1
if (-not $exe) { throw "No up-to-date game build found under $base. Open the new KirbyRecomp.exe once and wait until Play is enabled (or press HOST on the ONLINE page: it rebuilds the fast mode), then run this again." }
if (-not $Rom) {
    $last = Join-Path $base 'last_rom.txt'
    if (Test-Path $last) { $Rom = (Get-Content $last -Raw).Trim() }
}
if (-not $Rom -or -not (Test-Path $Rom)) { throw "ROM not found. Pass -Rom path\to\baserom.gba" }

$dir = Join-Path $env:TEMP 'kirby-local-online'
New-Item -ItemType Directory -Force $dir | Out-Null
$realSav = [System.IO.Path]::ChangeExtension($Rom, '.sav')
foreach ($n in 'host', 'join') {
    Set-Content -Path (Join-Path $dir "$n.ini") -Value '' -Encoding ascii
    $sav = Join-Path $dir "$n.sav"
    if (-not (Test-Path $sav) -and (Test-Path $realSav)) { Copy-Item $realSav $sav }   # start from your save, as a copy
}
function Q($s) { '"' + $s + '"' }
$common = @('--link-port', $Port, '--link-required', '--link-delay', $Delay)
$hostArgs = @((Q $Rom), '--sav', (Q (Join-Path $dir 'host.sav')), '--ini', (Q (Join-Path $dir 'host.ini')),
    '--link', 'host', '--link-players', 2, '--link-status', (Q (Join-Path $dir 'host.json'))) + $common
$joinArgs = @((Q $Rom), '--sav', (Q (Join-Path $dir 'join.sav')), '--ini', (Q (Join-Path $dir 'join.ini')),
    '--link', 'join', '--link-host', '127.0.0.1', '--link-status', (Q (Join-Path $dir 'join.json')), '--mute') + $common

Write-Host "exe : $($exe.FullName)"
Write-Host "rom : $Rom"
Write-Host "dir : $dir   (saves, settings, status files and kirby.log of the two test instances)"
Start-Process -FilePath $exe.FullName -ArgumentList $hostArgs -WorkingDirectory $exe.DirectoryName
Start-Sleep -Seconds 2
Start-Process -FilePath $exe.FullName -ArgumentList $joinArgs -WorkingDirectory $exe.DirectoryName
Write-Host ''
Write-Host 'Two windows open in a few seconds (the host window appears when the joiner is connected).'
Write-Host 'In BOTH: title START > FILE 1 (A twice) > START GAME > DOWN, A (MULTI PLAYER).'
Write-Host 'Host: A at "OK?". Both: A at GAME PAK SELECTION. Default keys: X = A, Z = B, Enter = Start.'
Write-Host 'Close a window (or F1) to end its instance.'
