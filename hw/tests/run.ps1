# Compila e roda hw/tests/test_frame.c de forma independente de recomp/.
# Saidas em build/hw/ (frame_*.ppm, test_frame.exe).
$root = Resolve-Path "$PSScriptRoot\..\.."
$gcc = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter gcc.exe -ErrorAction SilentlyContinue |
       Select-Object -First 1 -ExpandProperty FullName
if (-not $gcc) { $gcc = (Get-Command gcc -ErrorAction Stop).Source }
$out = Join-Path $root "build\hw"
New-Item -ItemType Directory -Force $out | Out-Null
& $gcc -std=c11 -O1 -Wall -Wextra -o "$out\test_frame.exe" "$root\hw\hw.c" "$root\hw\tests\test_frame.c"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& "$out\test_frame.exe" $out
exit $LASTEXITCODE
